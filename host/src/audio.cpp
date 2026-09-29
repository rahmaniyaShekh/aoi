#include "audio.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "dsp.h"
#include "util.h"

namespace aoi {

std::atomic<int> g_default_render_changes{0}, g_default_capture_changes{0};

namespace {

// ---- definitions missing from MinGW's headers ------------------------------------------
enum AOI_ACTIVATION_TYPE { AOI_ACTIVATION_TYPE_DEFAULT = 0, AOI_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 };
enum AOI_LOOPBACK_MODE { AOI_LOOPBACK_INCLUDE_TREE = 0, AOI_LOOPBACK_EXCLUDE_TREE = 1 };
struct AOI_ACTIVATION_PARAMS {
  DWORD ActivationType;
  DWORD TargetProcessId;
  DWORD ProcessLoopbackMode;
};
// PKEY_Device_FriendlyName (MinGW has no import library entry for it)
const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr REFERENCE_TIME kHns = 10000;  // per ms

template <class T> struct Com {
  T *p = nullptr;
  ~Com() { if (p) p->Release(); }
  T **operator&() { return &p; }
  T *operator->() { return p; }
  explicit operator bool() const { return p != nullptr; }
  void reset() { if (p) p->Release(); p = nullptr; }
};

struct ComInit {
  ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~ComInit() { CoUninitialize(); }
};

struct MmcssScope {
  HANDLE h = nullptr;
  explicit MmcssScope(const wchar_t *task) {
    DWORD idx = 0;
    h = AvSetMmThreadCharacteristicsW(task, &idx);
    if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
  }
  ~MmcssScope() { if (h) AvRevertMmThreadCharacteristics(h); }
};

// Activation completion handler. The API insists on an agile object.
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler {
 public:
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~ActivateHandler() { CloseHandle(done); }
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
        riid == kIID_IAgileObject) {
      *ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override {
    SetEvent(done);
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

using ActivateFn = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *,
                                     IActivateAudioInterfaceAsyncOperation **);

HRESULT activate_process_loopback(IAudioClient **out) {
  static ActivateFn fn = [] {
    HMODULE m = LoadLibraryW(L"Mmdevapi.dll");
    return m ? reinterpret_cast<ActivateFn>(GetProcAddress(m, "ActivateAudioInterfaceAsync")) : nullptr;
  }();
  if (!fn) return E_NOTIMPL;
  AOI_ACTIVATION_PARAMS params{AOI_ACTIVATION_TYPE_PROCESS_LOOPBACK, GetCurrentProcessId(), AOI_LOOPBACK_EXCLUDE_TREE};
  PROPVARIANT pv{};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof params;
  pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
  auto *handler = new ActivateHandler();
  IActivateAudioInterfaceAsyncOperation *op = nullptr;
  HRESULT hr = fn(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, handler, &op);
  if (SUCCEEDED(hr)) {
    if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) hr = E_FAIL;
    else {
      HRESULT act = E_FAIL;
      IUnknown *unk = nullptr;
      hr = op->GetActivateResult(&act, &unk);
      if (SUCCEEDED(hr)) hr = act;
      if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(out));
      if (unk) unk->Release();
    }
  }
  if (op) op->Release();
  handler->Release();
  return hr;
}

// Describes a device's mix format so any shared-mode stream can be converted.
struct Fmt {
  int channels = 2, rate = 48000, bytes = 4, block = 8;
  bool is_float = true;
  bool parse(const WAVEFORMATEX *w) {
    channels = w->nChannels;
    rate = int(w->nSamplesPerSec);
    bytes = w->wBitsPerSample / 8;
    block = w->nBlockAlign;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = true;
    else if (w->wFormatTag == WAVE_FORMAT_PCM) is_float = false;
    else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      auto *x = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(w);
      if (x->SubFormat == kSubtypeFloat) is_float = true;
      else if (x->SubFormat == kSubtypePcm) is_float = false;
      else return false;
      bytes = w->wBitsPerSample / 8;
      if (x->Samples.wValidBitsPerSample == 24 && bytes == 4) bytes = 4;  // 24-in-32
    } else return false;
    return channels > 0 && (bytes == 2 || bytes == 3 || bytes == 4);
  }
  float sample(const uint8_t *p) const {
    if (is_float) { float f; memcpy(&f, p, 4); return f; }
    if (bytes == 2) { int16_t v; memcpy(&v, p, 2); return v / 32768.f; }
    if (bytes == 3) { int32_t v = (int32_t(p[2]) << 24 | int32_t(p[1]) << 16 | int32_t(p[0]) << 8) >> 8; return v / 8388608.f; }
    int32_t v; memcpy(&v, p, 4); return float(v / 2147483648.0);
  }
  void put(uint8_t *p, float f) const {
    f = std::clamp(f, -1.f, 1.f);
    if (is_float) { memcpy(p, &f, 4); return; }
    if (bytes == 2) { int16_t v = int16_t(std::lrint(f * 32767.f)); memcpy(p, &v, 2); return; }
    if (bytes == 3) { int32_t v = int32_t(std::lrint(f * 8388607.f)); p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); return; }
    int32_t v = int32_t(std::lrint(double(f) * 2147483647.0)); memcpy(p, &v, 4);
  }
  // To interleaved stereo (downmix: front pair plus centre at -3 dB).
  void to_stereo(const uint8_t *src, int frames, std::vector<float> &out) const {
    out.resize(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
      const uint8_t *f = src + size_t(i) * block;
      float l = sample(f), r = channels > 1 ? sample(f + bytes) : l;
      if (channels >= 6) { float c = sample(f + 2 * bytes) * 0.7071f; l += c; r += c; }
      out[2 * i] = l; out[2 * i + 1] = r;
    }
  }
  void to_mono(const uint8_t *src, int frames, std::vector<float> &out) const {
    out.resize(size_t(frames));
    for (int i = 0; i < frames; ++i) {
      const uint8_t *f = src + size_t(i) * block;
      float s = 0;
      int n = std::min(channels, 2);
      for (int c = 0; c < n; ++c) s += sample(f + c * bytes);
      out[i] = s / float(n);
    }
  }
};

std::string device_name(IMMDevice *dev) {
  Com<IPropertyStore> props;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return "audio device";
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string name = "audio device";
  if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow(v.pwszVal);
  PropVariantClear(&v);
  return name;
}

// Tracks default-device changes for the loopback fallback and the talkback output.
class DeviceWatcher : public IMMNotificationClient {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) { *ppv = this; return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (role == eConsole) (flow == eRender ? g_default_render_changes : g_default_capture_changes)++;
    return S_OK;
  }
  STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
  STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

}  // namespace

void audio_watch_devices() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      IMMDeviceEnumerator *e = nullptr;
      if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                     reinterpret_cast<void **>(&e)))) {
        static DeviceWatcher w;
        e->RegisterEndpointNotificationCallback(&w);  // lives for the process
      }
      for (;;) Sleep(INFINITE);
    }).detach();
  });
}

// ======================================================================================
// SystemCapture
// ======================================================================================
void SystemCapture::start() {
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void SystemCapture::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void SystemCapture::emit(const float *x, int frames) {
  // Re-block to exactly kBlock frames: every stage downstream works in 10 ms.
  acc_.insert(acc_.end(), x, x + size_t(frames) * 2);
  size_t at = 0;
  while (acc_.size() - at >= size_t(kBlock) * 2) {
    cb_(acc_.data() + at, kBlock);
    at += size_t(kBlock) * 2;
    ++blocks_;
  }
  acc_.erase(acc_.begin(), acc_.begin() + long(at));
}

void SystemCapture::run() {
  ComInit com;
  MmcssScope mm(L"Pro Audio");
  audio_watch_devices();
  while (!stop_) {
    if (run_process_loopback()) continue;  // returns true on a recoverable stop
    if (stop_) break;
    run_endpoint_loopback();
  }
}

bool SystemCapture::run_process_loopback() {
  Com<IAudioClient> client;
  HRESULT hr = activate_process_loopback(&client);
  if (FAILED(hr)) {
    LOGW("process loopback unavailable: %s; using endpoint loopback", hr_text(hr).c_str());
    return false;
  }
  // The pseudo-device has no mix format: we declare one and the engine converts.
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  fmt.nChannels = 2;
  fmt.nSamplesPerSec = kRate;
  fmt.wBitsPerSample = 32;
  fmt.nBlockAlign = 8;
  fmt.nAvgBytesPerSec = kRate * 8;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                          100 * kHns, 0, &fmt, nullptr);
  if (FAILED(hr)) {
    LOGW("process loopback Initialize failed: %s", hr_text(hr).c_str());
    return false;
  }
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  client->SetEventHandle(ev);
  Com<IAudioCaptureClient> cap;
  client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap));
  if (!cap || FAILED(client->Start())) {
    CloseHandle(ev);
    return false;
  }
  process_loopback_ = true;
  { std::lock_guard lk(mu_); source_ = "All apps except AOI (process loopback)"; }
  LOGI("capturing: process loopback, excluding pid %lu", GetCurrentProcessId());

  std::vector<float> silence;
  int64_t last_data = now_us();
  while (!stop_) {
    WaitForSingleObject(ev, 20);
    UINT32 pending = 0;
    bool got = false;
    while (SUCCEEDED(hr = cap->GetNextPacketSize(&pending)) && pending > 0) {
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      if (frames) {
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
          silence.assign(size_t(frames) * 2, 0.f);
          emit(silence.data(), int(frames));
        } else {
          emit(reinterpret_cast<const float *>(data), int(frames));
        }
        got = true;
      }
      cap->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) { LOGW("process loopback read failed: %s", hr_text(hr).c_str()); break; }
    int64_t now = now_us();
    if (got) last_data = now;
    else if (now - last_data > 30000) {
      // The engine stopped delivering (it can while nothing plays): keep the
      // clock running with silence so listeners' buffers stay primed.
      int frames = int((now - last_data) * kRate / 1000000);
      frames -= frames % kBlock;
      if (frames > 0) {
        silence.assign(size_t(frames) * 2, 0.f);
        emit(silence.data(), frames);
        last_data += int64_t(frames) * 1000000 / kRate;
      }
    }
  }
  client->Stop();
  CloseHandle(ev);
  process_loopback_ = false;
  if (!stop_) { ++restarts_; Sleep(200); }
  return true;
}

void SystemCapture::run_endpoint_loopback() {
  Com<IMMDeviceEnumerator> en;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(&en)))) {
    Sleep(1000);
    return;
  }
  Com<IMMDevice> dev;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    { std::lock_guard lk(mu_); source_ = "no output device"; }
    std::vector<float> z(size_t(kBlock) * 2, 0.f);
    for (int i = 0; i < 100 && !stop_; ++i) { emit(z.data(), kBlock); Sleep(10); }
    return;
  }
  std::string name = device_name(dev.p);
  Com<IAudioClient> client;
  WAVEFORMATEX *mix = nullptr;
  Fmt f;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) ||
      FAILED(client->GetMixFormat(&mix)) || !f.parse(mix) ||
      FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200 * kHns, 0, mix, nullptr))) {
    if (mix) CoTaskMemFree(mix);
    LOGW("endpoint loopback failed on %s", name.c_str());
    Sleep(1000);
    return;
  }
  CoTaskMemFree(mix);
  Com<IAudioCaptureClient> cap;
  client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap));
  if (!cap || FAILED(client->Start())) { Sleep(1000); return; }
  { std::lock_guard lk(mu_); source_ = name + " (endpoint loopback)"; }
  LOGI("capturing: endpoint loopback on %s, %d Hz %d ch", name.c_str(), f.rate, f.channels);

  Resampler rs(2, f.rate, kRate);
  std::vector<float> st, out, silence;
  int changes = g_default_render_changes;
  int64_t last_data = now_us();
  HRESULT hr = S_OK;
  while (!stop_ && changes == g_default_render_changes) {
    Sleep(5);
    UINT32 pending = 0;
    bool got = false;
    while (SUCCEEDED(hr = cap->GetNextPacketSize(&pending)) && pending > 0) {
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      if (frames) {
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) st.assign(size_t(frames) * 2, 0.f);
        else f.to_stereo(data, int(frames), st);
        out.clear();
        rs.process(st.data(), int(frames), out);
        if (!out.empty()) emit(out.data(), int(out.size() / 2));
        got = true;
      }
      cap->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) break;  // device invalidated: reopen on whatever is default now
    int64_t now = now_us();
    if (got) last_data = now;
    else if (now - last_data > 30000) {
      // Endpoint loopback delivers nothing while nothing plays.
      int frames = int((now - last_data) * kRate / 1000000);
      frames -= frames % kBlock;
      if (frames > 0) {
        silence.assign(size_t(frames) * 2, 0.f);
        emit(silence.data(), frames);
        last_data += int64_t(frames) * 1000000 / kRate;
      }
    }
  }
  client->Stop();
  if (!stop_) {
    ++restarts_;
    LOGI("output device changed; following it");
  }
}

// ======================================================================================
// MicCapture
// ======================================================================================
void MicCapture::start() {
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}
void MicCapture::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void MicCapture::run() {
  ComInit com;
  MmcssScope mm(L"Pro Audio");
  audio_watch_devices();
  int backoff = 500;
  while (!stop_) {
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    Com<IAudioClient> client;
    Com<IAudioCaptureClient> cap;
    WAVEFORMATEX *mix = nullptr;
    Fmt f;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) &&
              SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) &&
              SUCCEEDED(client->GetMixFormat(&mix)) && f.parse(mix) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 50 * kHns, 0,
                                           mix, nullptr)) &&
              SUCCEEDED(client->SetEventHandle(ev)) &&
              SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap))) &&
              SUCCEEDED(client->Start());
    if (mix) CoTaskMemFree(mix);
    if (!ok) {
      CloseHandle(ev);
      { std::lock_guard lk(mu_); name_ = "no microphone"; }
      running_ = false;
      for (int i = 0; i < backoff / 50 && !stop_; ++i) Sleep(50);
      backoff = std::min(backoff * 2, 10000);
      continue;
    }
    backoff = 500;
    { std::lock_guard lk(mu_); name_ = device_name(dev.p); }
    LOGI("microphone: %s, %d Hz %d ch", name().c_str(), f.rate, f.channels);
    running_ = true;
    Resampler rs(1, f.rate, kRate);
    std::vector<float> mono, out;
    int changes = g_default_capture_changes;
    HRESULT hr = S_OK;
    while (!stop_ && changes == g_default_capture_changes) {
      WaitForSingleObject(ev, 50);
      UINT32 pending = 0;
      while (SUCCEEDED(hr = cap->GetNextPacketSize(&pending)) && pending > 0) {
        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (frames) {
          if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) mono.assign(frames, 0.f);
          else f.to_mono(data, int(frames), mono);
          out.clear();
          rs.process(mono.data(), int(frames), out);
          if (!out.empty()) cb_(out.data(), int(out.size()));
        }
        cap->ReleaseBuffer(frames);
      }
      if (FAILED(hr)) break;
    }
    client->Stop();
    CloseHandle(ev);
    running_ = false;
  }
}

// ======================================================================================
// Render (talkback output)
// ======================================================================================
void Render::start() {
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}
void Render::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void Render::run() {
  ComInit com;
  MmcssScope mm(L"Pro Audio");
  audio_watch_devices();
  while (!stop_) {
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    Com<IAudioClient> client;
    Com<IAudioRenderClient> rc;
    WAVEFORMATEX *mix = nullptr;
    Fmt f;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 bufFrames = 0;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) &&
              SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) &&
              SUCCEEDED(client->GetMixFormat(&mix)) && f.parse(mix) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 40 * kHns, 0,
                                           mix, nullptr)) &&
              SUCCEEDED(client->SetEventHandle(ev)) && SUCCEEDED(client->GetBufferSize(&bufFrames)) &&
              SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&rc)));
    if (mix) CoTaskMemFree(mix);
    if (!ok) {
      CloseHandle(ev);
      running_ = false;
      for (int i = 0; i < 20 && !stop_; ++i) Sleep(50);
      continue;
    }
    { std::lock_guard lk(mu_); name_ = device_name(dev.p); }
    // Prime with silence so the first period does not glitch.
    BYTE *buf = nullptr;
    if (SUCCEEDED(rc->GetBuffer(bufFrames, &buf))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    client->Start();
    running_ = true;
    LOGI("talkback output: %s, %d Hz %d ch", name().c_str(), f.rate, f.channels);
    Resampler rs(1, kRate, f.rate);
    std::vector<float> pulled, fifo;
    int changes = g_default_render_changes;
    while (!stop_ && changes == g_default_render_changes) {
      WaitForSingleObject(ev, 100);
      UINT32 padding = 0;
      if (FAILED(client->GetCurrentPadding(&padding))) break;
      UINT32 want = bufFrames - padding;
      if (want == 0) continue;
      while (fifo.size() < want) {
        int n = int((int64_t(want - fifo.size()) * kRate + f.rate - 1) / f.rate);
        n = std::max(n, 48);
        pulled.assign(size_t(n), 0.f);
        pull_(pulled.data(), n);
        rs.process(pulled.data(), n, fifo);
      }
      if (FAILED(rc->GetBuffer(want, &buf))) break;
      for (UINT32 i = 0; i < want; ++i)
        for (int c = 0; c < f.channels; ++c) f.put(buf + size_t(i) * f.block + size_t(c) * f.bytes, fifo[i]);
      rc->ReleaseBuffer(want, 0);
      fifo.erase(fifo.begin(), fifo.begin() + want);
    }
    client->Stop();
    CloseHandle(ev);
    running_ = false;
  }
}

}  // namespace aoi
