// platform/audio_system.h on Windows: WASAPI.
//
// The system capture taps "what this PC is playing" with *process loopback*
// in EXCLUDE mode against our own process tree: every application's audio,
// taken before the endpoint's effects chain (no driver compressor), endpoint
// agnostic (Bluetooth connecting mid-song changes nothing, and a headset that
// drops to call quality while its mic is open does not degrade what listeners
// get), and structurally free of anything we play ourselves -- which is what
// makes talkback on a single output device possible without the listener
// hearing themselves. If process loopback is refused it falls back to
// endpoint loopback that follows the default output device.
//
// Microphones, talkback output and the echo reference (endpoint loopback of
// the talkback device) open a specific endpoint id, or follow the Windows
// default when the id is empty.
#include "platform/audio_system.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/dsp.h"
#include "core/util.h"

namespace aoi {

namespace {

// ---- definitions missing from MinGW's headers ------------------------------------------
enum AOI_ACTIVATION_TYPE { AOI_ACTIVATION_TYPE_DEFAULT = 0, AOI_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 };
enum AOI_LOOPBACK_MODE { AOI_LOOPBACK_INCLUDE_TREE = 0, AOI_LOOPBACK_EXCLUDE_TREE = 1 };
struct AOI_ACTIVATION_PARAMS {
  DWORD ActivationType;
  DWORD TargetProcessId;
  DWORD ProcessLoopbackMode;
};
// MinGW has no import-library entries for these PROPERTYKEYs.
const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyEnumeratorName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
const PROPERTYKEY kPkeyFormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
const PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
// The endpoint's device instance path, and (for Bluetooth audio offloaded to
// the chipset's DSP, where the enumerator says INTELAUDIO) the Bluetooth
// device it belongs to; plus the kernel-streaming filter path.
const PROPERTYKEY kPkeyDeviceInstance = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};
const PROPERTYKEY kPkeyDeviceRelated = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 39};
const PROPERTYKEY kPkeyKsFilterPath = {{0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1};
// PKEY_Devices_AudioDevice_RawProcessingSupported: the endpoint can deliver
// its signal without the driver's (or an add-on suite's) effects.
const PROPERTYKEY kPkeyRawSupported = {{0x8943b373, 0x388c, 0x4395, {0xb5, 0x57, 0xbc, 0x6d, 0xba, 0xff, 0xaf, 0xdb}}, 2};
const GUID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// The container every built-in device of this PC shares.
const char *kPcContainer = "{00000000-0000-0000-ffff-ffffffffffff}";
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

// Waits up to `ms` in short steps so stop() stays prompt.
void nap(const std::atomic<bool> &stop, int ms) {
  for (int i = 0; i < ms / 20 && !stop; ++i) Sleep(20);
}

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
    } else return false;
    if (is_float && bytes != 4) return false;
    return channels > 0 && rate > 0 && (bytes == 2 || bytes == 3 || bytes == 4) && block >= channels * bytes;
  }
  std::string describe() const {
    return is_float ? "32-bit float" : std::to_string(bytes * 8) + "-bit PCM";
  }
  float sample(const uint8_t *p) const {
    if (is_float) { float f; memcpy(&f, p, 4); return f; }
    if (bytes == 2) { int16_t v; memcpy(&v, p, 2); return v / 32768.f; }
    if (bytes == 3) { int32_t v = (int32_t(p[2]) << 24 | int32_t(p[1]) << 16 | int32_t(p[0]) << 8) >> 8; return v / 8388608.f; }
    int32_t v; memcpy(&v, p, 4); return float(v / 2147483648.0);  // 32-bit and 24-in-32 (left-justified)
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
  // Every channel averaged: a stereo mic, a 2- or 4-element array, or a
  // render mix (the echo reference) all fold to one honest mono signal.
  void to_mono(const uint8_t *src, int frames, std::vector<float> &out) const {
    out.resize(size_t(frames));
    const float k = 1.f / float(channels);
    for (int i = 0; i < frames; ++i) {
      const uint8_t *f = src + size_t(i) * block;
      float s = 0;
      for (int c = 0; c < channels; ++c) s += sample(f + c * bytes);
      out[i] = s * k;
    }
  }
};

std::string prop_string(IPropertyStore *ps, const PROPERTYKEY &k) {
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string s;
  if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = narrow(v.pwszVal);
  PropVariantClear(&v);
  return s;
}

std::string device_name(IMMDevice *dev) {
  Com<IPropertyStore> props;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return "audio device";
  std::string n = prop_string(props.p, kPkeyFriendlyName);
  return n.empty() ? "audio device" : n;
}

std::string device_id(IMMDevice *dev) {
  LPWSTR id = nullptr;
  std::string s;
  if (SUCCEEDED(dev->GetId(&id)) && id) s = narrow(id);
  CoTaskMemFree(id);
  return s;
}

std::string guid_text(const GUID &g) {
  char b[48];
  snprintf(b, sizeof b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)g.Data1, g.Data2, g.Data3,
           g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return b;
}

FormFactor form_of(UINT v) {
  switch (v) {
    case 0: return FormFactor::Network;
    case 1: return FormFactor::Speakers;
    case 2: return FormFactor::LineLevel;
    case 3: return FormFactor::Headphones;
    case 4: return FormFactor::Microphone;
    case 5: return FormFactor::Headset;
    case 6: return FormFactor::Handset;
    case 7: case 8: return FormFactor::Digital;
    case 9: return FormFactor::Hdmi;
    default: return FormFactor::Unknown;
  }
}

std::string upper(std::string s) {
  for (auto &c : s) c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
  return s;
}

DeviceInfo describe(IMMDevice *dev, DeviceKind kind) {
  DeviceInfo d;
  d.kind = kind;
  d.id = device_id(dev);
  Com<IPropertyStore> ps;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) { d.name = "audio device"; return d; }
  d.name = prop_string(ps.p, kPkeyFriendlyName);
  if (d.name.empty()) d.name = "audio device";
  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(ps->GetValue(kPkeyFormFactor, &v)) && v.vt == VT_UI4) d.form = form_of(v.ulVal);
  PropVariantClear(&v);
  PropVariantInit(&v);
  if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) d.container = guid_text(*v.puuid);
  PropVariantClear(&v);
  d.builtin_container = d.container == kPcContainer;
  // Where the endpoint really lives. Bluetooth audio offloaded to the
  // chipset (Intel Smart Sound) reports INTELAUDIO as its enumerator; the
  // Bluetooth device and its profile UUID show up in the related path.
  std::string where = upper(prop_string(ps.p, kPkeyEnumeratorName) + "|" + prop_string(ps.p, kPkeyDeviceInstance) + "|" +
                            prop_string(ps.p, kPkeyDeviceRelated) + "|" + prop_string(ps.p, kPkeyKsFilterPath));
  auto has = [&](const char *s) { return where.find(s) != std::string::npos; };
  if (has("BTHENUM") || has("BTHHFENUM") || has("BTHLEDEVICE") || has("BTHLE") || has("BTHA2DP")) {
    d.bus = DeviceBus::Bluetooth;
    d.bluetooth_le = has("BTHLE") || has("{0000184E") || has("{00001850");
    // Hands-Free (111E), Headset (1108), or the HFP audio enumerator.
    d.bluetooth_hfp = !d.bluetooth_le && (has("0000111E") || has("00001108") || has("BTHHFENUM"));
  } else if (has("USB\\") || has("|USB|") || where.rfind("USB", 0) == 0) {
    d.bus = DeviceBus::Usb;
  } else if (d.form == FormFactor::Hdmi) {
    d.bus = DeviceBus::Hdmi;
  } else if (has("ROOT\\") || where.rfind("ROOT", 0) == 0) {
    d.bus = DeviceBus::Virtual;
  } else if (d.builtin_container) {
    d.bus = DeviceBus::BuiltIn;
  }
  return d;
}

bool raw_supported(IMMDevice *dev) {
  Com<IPropertyStore> ps;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return false;
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = SUCCEEDED(ps->GetValue(kPkeyRawSupported, &v)) && v.vt == VT_BOOL && v.boolVal;
  PropVariantClear(&v);
  return ok;
}

// A microphone in raw mode: no driver AGC, noise suppression, "voice
// enhancement" or add-on effects suite (Nahimic, Waves, DTS...). Those are
// non-linear and change their gain all the time, which an echo canceller
// cannot follow; our own voice chain does the same jobs after it. Must be
// set before Initialize. False (default mode) when the device has no raw path.
bool request_raw(IAudioClient *client, IMMDevice *dev) {
  if (!raw_supported(dev)) return false;
  Com<IAudioClient2> c2;
  if (FAILED(client->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void **>(&c2)))) return false;
  AudioClientProperties p{};
  p.cbSize = sizeof p;
  p.bIsOffload = FALSE;
  p.eCategory = AudioCategory_Other;
  p.Options = AUDCLNT_STREAMOPTIONS_RAW;
  return SUCCEEDED(c2->SetClientProperties(&p));
}

HRESULT make_enumerator(Com<IMMDeviceEnumerator> &en) {
  return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void **>(&en));
}

// Opens `id`, or the current default endpoint of `flow` when id is empty.
HRESULT open_device(IMMDeviceEnumerator *en, const std::string &id, EDataFlow flow, IMMDevice **out) {
  if (id.empty()) return en->GetDefaultAudioEndpoint(flow, eConsole, out);
  HRESULT hr = en->GetDevice(widen(id).c_str(), out);
  if (FAILED(hr)) return hr;
  DWORD state = 0;
  (*out)->GetState(&state);
  if (state != DEVICE_STATE_ACTIVE) {
    (*out)->Release();
    *out = nullptr;
    return E_NOTFOUND;
  }
  return S_OK;
}

std::string open_error(HRESULT hr, bool capture, bool *denied, bool *missing) {
  if (hr == E_ACCESSDENIED) {
    if (denied) *denied = true;
    return capture ? "Windows is blocking microphone access for desktop apps: Settings → Privacy & security → Microphone"
                   : "access denied";
  }
  if (hr == E_NOTFOUND || hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
    if (missing) *missing = true;
    return "device not connected";
  }
  if (hr == AUDCLNT_E_DEVICE_IN_USE) return "device in use by another app in exclusive mode";
  if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "unsupported device format";
  return os_error_text(hr);
}

// Everyone who needs to know when devices come and go.
class Notifier final : public IMMNotificationClient {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) { *ppv = this; return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
    if (role == eConsole) (flow == eRender ? render_default_gen : capture_default_gen)++;
    emit(DeviceEvent::DefaultChanged, id);
    return S_OK;
  }
  STDMETHODIMP OnDeviceAdded(LPCWSTR id) override { emit(DeviceEvent::Added, id); return S_OK; }
  STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override { emit(DeviceEvent::Removed, id); return S_OK; }
  STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD) override { emit(DeviceEvent::StateChanged, id); return S_OK; }
  STDMETHODIMP OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
    // Only a rename matters to us (the rest churns constantly: volume, jack sensing...).
    if (key.fmtid == kPkeyFriendlyName.fmtid && key.pid == kPkeyFriendlyName.pid) emit(DeviceEvent::PropertyChanged, id);
    return S_OK;
  }

  int add(IAudioSystem::EventSink s) {
    std::lock_guard lk(mu_);
    sinks_[++next_] = std::move(s);
    return next_;
  }
  void remove(int token) {
    std::lock_guard lk(mu_);
    sinks_.erase(token);
  }

  std::atomic<int> render_default_gen{0}, capture_default_gen{0};

 private:
  void emit(DeviceEvent e, LPCWSTR id) {
    std::string s = id ? narrow(id) : std::string();
    std::lock_guard lk(mu_);
    for (auto &kv : sinks_) kv.second(e, s);
  }
  std::mutex mu_;
  std::map<int, IAudioSystem::EventSink> sinks_;
  int next_ = 0;
};

Notifier &notifier() {
  static Notifier n;
  static std::once_flag once;
  std::call_once(once, [] {
    // The registration lives for the process, on a thread of its own.
    std::thread([] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      IMMDeviceEnumerator *e = nullptr;
      if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                     reinterpret_cast<void **>(&e))))
        e->RegisterEndpointNotificationCallback(&n);
      for (;;) Sleep(INFINITE);
    }).detach();
  });
  return n;
}

// ---- common stream plumbing ----------------------------------------------------------------
class StreamBase {
 public:
  virtual ~StreamBase() = default;
  StreamStatus get() const {
    std::lock_guard lk(mu_);
    StreamStatus s = st_;
    s.frames = frames_;
    return s;
  }

 protected:
  void set_running(const std::string &name, const Fmt &f, double latency_ms) {
    std::lock_guard lk(mu_);
    st_.running = true;
    st_.device_name = name;
    st_.rate = f.rate;
    st_.channels = f.channels;
    st_.format = f.describe();
    st_.latency_ms = latency_ms;
    st_.error.clear();
    st_.access_denied = st_.missing = false;
  }
  void set_failed(const std::string &err, bool denied, bool missing) {
    std::lock_guard lk(mu_);
    st_.running = false;
    st_.error = err;
    st_.access_denied = denied;
    st_.missing = missing;
  }
  void set_stopped() {
    std::lock_guard lk(mu_);
    st_.running = false;
  }
  void join() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  mutable std::mutex mu_;
  StreamStatus st_;
  std::atomic<uint64_t> frames_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// ======================================================================================
// System capture
// ======================================================================================
class WinSystemCapture final : public SystemCaptureStream, StreamBase {
 public:
  explicit WinSystemCapture(IAudioSystem::StereoBlock cb) : cb_(std::move(cb)) {
    thread_ = std::thread([this] { run(); });
  }
  ~WinSystemCapture() override { join(); }
  StreamStatus status() const override { return get(); }
  std::string source() const override { std::lock_guard lk(mu_); return source_; }
  bool process_loopback() const override { return process_loopback_; }
  uint64_t blocks() const override { return blocks_; }

 private:
  void run() {
    ComInit com;
    MmcssScope mm(L"Pro Audio");
    notifier();
    while (!stop_) {
      if (run_process_loopback()) continue;  // returns true on a recoverable stop
      if (stop_) break;
      run_endpoint_loopback();
    }
  }

  void emit(const float *x, int frames) {
    // Re-block to exactly kBlock frames: every stage downstream works in 10 ms.
    acc_.insert(acc_.end(), x, x + size_t(frames) * 2);
    size_t at = 0;
    while (acc_.size() - at >= size_t(kBlock) * 2) {
      cb_(acc_.data() + at, kBlock);
      at += size_t(kBlock) * 2;
      ++blocks_;
      frames_ += kBlock;
    }
    acc_.erase(acc_.begin(), acc_.begin() + long(at));
  }

  bool run_process_loopback() {
    Com<IAudioClient> client;
    HRESULT hr = activate_process_loopback(&client);
    if (FAILED(hr)) {
      LOGW("process loopback unavailable: %s; using endpoint loopback", os_error_text(hr).c_str());
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
      LOGW("process loopback Initialize failed: %s", os_error_text(hr).c_str());
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
    Fmt f;
    set_running("process loopback", f, 0);
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
      if (FAILED(hr)) { LOGW("process loopback read failed: %s", os_error_text(hr).c_str()); break; }
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

  void run_endpoint_loopback() {
    Com<IMMDeviceEnumerator> en;
    if (FAILED(make_enumerator(en))) { Sleep(1000); return; }
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
    set_running(name, f, 0);
    LOGI("capturing: endpoint loopback on %s, %d Hz %d ch", name.c_str(), f.rate, f.channels);

    Resampler rs(2, f.rate, kRate);
    std::vector<float> st, out, silence;
    int changes = notifier().render_default_gen;
    int64_t last_data = now_us();
    HRESULT hr = S_OK;
    while (!stop_ && changes == notifier().render_default_gen) {
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

  IAudioSystem::StereoBlock cb_;
  std::atomic<bool> process_loopback_{false};
  std::atomic<uint64_t> blocks_{0};
  std::atomic<int> restarts_{0};
  std::string source_ = "starting";
  std::vector<float> acc_;
};

// ======================================================================================
// Capture of one endpoint: a microphone, or a render endpoint's loopback.
// ======================================================================================
class WinCapture final : public AudioStream, StreamBase {
 public:
  WinCapture(std::string id, bool loopback, IAudioSystem::MonoSink cb)
      : id_(std::move(id)), loopback_(loopback), cb_(std::move(cb)) {
    thread_ = std::thread([this] { run(); });
  }
  ~WinCapture() override { join(); }
  StreamStatus status() const override { return get(); }

 private:
  void run() {
    ComInit com;
    MmcssScope mm(L"Pro Audio");
    notifier();
    int backoff = 300;
    const char *what = loopback_ ? "echo reference" : "microphone";
    // Loopback cannot signal an event, so it is polled. With a 1 ms timer the
    // reference reaches the echo canceller within ~3 ms of the engine mixing
    // it, instead of up to a 15.6 ms default timer tick later.
    struct FineTimer {
      bool on;
      explicit FineTimer(bool o) : on(o) { if (on) timeBeginPeriod(1); }
      ~FineTimer() { if (on) timeEndPeriod(1); }
    } timer(loopback_);
    std::string last_err;
    while (!stop_) {
      bool denied = false, missing = false;
      HRESULT hr = S_OK;
      Com<IMMDeviceEnumerator> en;
      Com<IMMDevice> dev;
      Com<IAudioClient> client;
      Com<IAudioCaptureClient> cap;
      WAVEFORMATEX *mix = nullptr;
      Fmt f;
      HANDLE ev = loopback_ ? nullptr : CreateEventW(nullptr, FALSE, FALSE, nullptr);
      int gen = loopback_ ? notifier().render_default_gen.load() : notifier().capture_default_gen.load();
      bool raw = false;
      if (FAILED(hr = make_enumerator(en)) || FAILED(hr = open_device(en.p, id_, loopback_ ? eRender : eCapture, &dev)) ||
          FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) ||
          ((raw = !loopback_ && request_raw(client.p, dev.p)), false) ||
          FAILED(hr = client->GetMixFormat(&mix))) {
      } else if (!f.parse(mix)) {
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
      } else if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                (loopback_ ? 100 : 50) * kHns, 0, mix, nullptr)) ||
                 (ev && FAILED(hr = client->SetEventHandle(ev))) ||
                 FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap))) ||
                 FAILED(hr = client->Start())) {
      }
      if (mix) CoTaskMemFree(mix);
      if (FAILED(hr)) {
        if (ev) CloseHandle(ev);
        std::string err = open_error(hr, !loopback_, &denied, &missing);
        set_failed(err, denied, missing);
        if (err != last_err) LOGW("%s %s: %s", what, id_.empty() ? "(default)" : dev ? device_name(dev.p).c_str() : id_.c_str(), err.c_str());
        last_err = err;
        nap(stop_, backoff);
        backoff = std::min(backoff * 2, 3000);
        continue;
      }
      backoff = 300;
      last_err.clear();
      std::string name = device_name(dev.p);
      REFERENCE_TIME lat = 0, period = 0;
      client->GetStreamLatency(&lat);
      client->GetDevicePeriod(&period, nullptr);
      set_running(name, f, double(lat + period) / kHns);
      if (raw) { std::lock_guard lk(mu_); st_.format += ", raw"; }
      LOGI("%s: %s, %d Hz %d ch %s%s", what, name.c_str(), f.rate, f.channels, f.describe().c_str(),
           raw ? ", raw (no driver effects)" : "");
      Resampler rs(1, f.rate, kRate);
      std::vector<float> mono, out, silence;
      int64_t last_data = now_us();
      std::atomic<int> &gen_now = loopback_ ? notifier().render_default_gen : notifier().capture_default_gen;
      while (!stop_ && (!id_.empty() || gen == gen_now)) {
        if (ev) WaitForSingleObject(ev, 50);
        else Sleep(2);
        UINT32 pending = 0;
        bool got = false;
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
            if (!out.empty()) { cb_(out.data(), int(out.size())); frames_ += out.size(); }
            got = true;
          }
          cap->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) break;
        if (loopback_) {
          // Loopback delivers nothing while nothing plays: keep the reference's
          // clock running with silence (the echo canceller expects it).
          int64_t now = now_us();
          if (got) last_data = now;
          else if (now - last_data > 30000) {
            int frames = int((now - last_data) * kRate / 1000000);
            frames -= frames % kBlock;
            if (frames > 0) {
              silence.assign(size_t(frames), 0.f);
              cb_(silence.data(), frames);
              frames_ += uint64_t(frames);
              last_data += int64_t(frames) * 1000000 / kRate;
            }
          }
        }
      }
      client->Stop();
      if (ev) CloseHandle(ev);
      if (FAILED(hr)) {
        std::string err = open_error(hr, !loopback_, &denied, &missing);
        set_failed(err, denied, missing);
        LOGW("%s %s stopped: %s", what, name.c_str(), err.c_str());
        nap(stop_, 300);
      } else {
        set_stopped();
      }
    }
    set_stopped();
  }

  std::string id_;
  bool loopback_;
  IAudioSystem::MonoSink cb_;
};

// ======================================================================================
// Render (talkback output)
// ======================================================================================
class WinRender final : public AudioStream, StreamBase {
 public:
  WinRender(std::string id, IAudioSystem::MonoPull pull) : id_(std::move(id)), pull_(std::move(pull)) {
    thread_ = std::thread([this] { run(); });
  }
  ~WinRender() override { join(); }
  StreamStatus status() const override { return get(); }

 private:
  void run() {
    ComInit com;
    MmcssScope mm(L"Pro Audio");
    notifier();
    std::string last_err;
    while (!stop_) {
      HRESULT hr = S_OK;
      Com<IMMDeviceEnumerator> en;
      Com<IMMDevice> dev;
      Com<IAudioClient> client;
      Com<IAudioRenderClient> rc;
      WAVEFORMATEX *mix = nullptr;
      Fmt f;
      HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      UINT32 bufFrames = 0;
      int gen = notifier().render_default_gen;
      if (FAILED(hr = make_enumerator(en)) || FAILED(hr = open_device(en.p, id_, eRender, &dev)) ||
          FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) ||
          FAILED(hr = client->GetMixFormat(&mix))) {
      } else if (!f.parse(mix)) {
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
      } else if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 40 * kHns, 0,
                                                mix, nullptr)) ||
                 FAILED(hr = client->SetEventHandle(ev)) || FAILED(hr = client->GetBufferSize(&bufFrames)) ||
                 FAILED(hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&rc)))) {
      }
      if (mix) CoTaskMemFree(mix);
      if (FAILED(hr)) {
        CloseHandle(ev);
        bool denied = false, missing = false;
        std::string err = open_error(hr, false, &denied, &missing);
        set_failed(err, denied, missing);
        if (err != last_err) LOGW("talkback output: %s", err.c_str());
        last_err = err;
        nap(stop_, 1000);
        continue;
      }
      last_err.clear();
      std::string name = device_name(dev.p);
      // Prime with silence so the first period does not glitch.
      BYTE *buf = nullptr;
      if (SUCCEEDED(rc->GetBuffer(bufFrames, &buf))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
      client->Start();
      REFERENCE_TIME lat = 0;
      client->GetStreamLatency(&lat);
      set_running(name, f, double(lat) / kHns + double(bufFrames) * 1000.0 / f.rate);
      LOGI("talkback output: %s, %d Hz %d ch", name.c_str(), f.rate, f.channels);
      Resampler rs(1, kRate, f.rate);
      std::vector<float> pulled, fifo;
      while (!stop_ && (!id_.empty() || gen == notifier().render_default_gen)) {
        WaitForSingleObject(ev, 100);
        UINT32 padding = 0;
        if (FAILED(hr = client->GetCurrentPadding(&padding))) break;
        UINT32 want = bufFrames - padding;
        if (want == 0) continue;
        while (fifo.size() < want) {
          int n = int((int64_t(want - fifo.size()) * kRate + f.rate - 1) / f.rate);
          n = std::max(n, 48);
          pulled.assign(size_t(n), 0.f);
          pull_(pulled.data(), n);
          frames_ += uint64_t(n);
          rs.process(pulled.data(), n, fifo);
        }
        if (FAILED(hr = rc->GetBuffer(want, &buf))) break;
        for (UINT32 i = 0; i < want; ++i)
          for (int c = 0; c < f.channels; ++c) f.put(buf + size_t(i) * f.block + size_t(c) * f.bytes, fifo[i]);
        rc->ReleaseBuffer(want, 0);
        fifo.erase(fifo.begin(), fifo.begin() + want);
      }
      client->Stop();
      CloseHandle(ev);
      if (FAILED(hr)) {
        bool denied = false, missing = false;
        set_failed(open_error(hr, false, &denied, &missing), denied, missing);
        nap(stop_, 300);
      } else {
        set_stopped();
      }
    }
    set_stopped();
  }

  std::string id_;
  IAudioSystem::MonoPull pull_;
};

// ======================================================================================
// The system
// ======================================================================================
class WasapiSystem final : public IAudioSystem {
 public:
  bool supported() const override { return true; }
  std::string backend() const override { return "WASAPI"; }

  std::vector<DeviceInfo> list_devices(DeviceKind kind) override {
    ComInit com;
    std::vector<DeviceInfo> out;
    Com<IMMDeviceEnumerator> en;
    if (FAILED(make_enumerator(en))) return out;
    EDataFlow flow = kind == DeviceKind::Capture ? eCapture : eRender;
    std::string def_console, def_comms;
    {
      Com<IMMDevice> d;
      if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) def_console = device_id(d.p);
    }
    {
      Com<IMMDevice> d;
      if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eCommunications, &d))) def_comms = device_id(d.p);
    }
    Com<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      Com<IMMDevice> d;
      if (FAILED(col->Item(i, &d))) continue;
      DeviceInfo info = describe(d.p, kind);
      info.default_console = info.id == def_console;
      info.default_comms = info.id == def_comms;
      out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const DeviceInfo &a, const DeviceInfo &b) {
      return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    return out;
  }

  std::unique_ptr<SystemCaptureStream> open_system_capture(StereoBlock cb) override {
    return std::make_unique<WinSystemCapture>(std::move(cb));
  }
  std::unique_ptr<AudioStream> open_mic(const std::string &id, MonoSink cb) override {
    return std::make_unique<WinCapture>(id, false, std::move(cb));
  }
  std::unique_ptr<AudioStream> open_render(const std::string &id, MonoPull pull) override {
    return std::make_unique<WinRender>(id, std::move(pull));
  }
  std::unique_ptr<AudioStream> open_render_loopback(const std::string &id, MonoSink cb) override {
    return std::make_unique<WinCapture>(id, true, std::move(cb));
  }

  int subscribe(EventSink sink) override { return notifier().add(std::move(sink)); }
  void unsubscribe(int token) override { notifier().remove(token); }

  std::optional<float> input_volume(const std::string &id) override {
    ComInit com;
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    Com<IAudioEndpointVolume> vol;
    float v = 0;
    if (FAILED(make_enumerator(en)) || FAILED(open_device(en.p, id, eCapture, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&vol))) ||
        FAILED(vol->GetMasterVolumeLevelScalar(&v)))
      return std::nullopt;
    BOOL mute = FALSE;
    if (SUCCEEDED(vol->GetMute(&mute)) && mute) return 0.f;
    return v;
  }
};

}  // namespace

std::unique_ptr<IAudioSystem> create_audio_system() { return std::make_unique<WasapiSystem>(); }

}  // namespace aoi
