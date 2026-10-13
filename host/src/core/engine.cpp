#include "engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "crypto.h"
#include "util.h"

using json = nlohmann::json;

namespace aoi {

// One open microphone with its own voice processor. The stream's thread
// re-blocks to 10 ms, processes, and writes into `reader`, which the
// system-capture thread reads on its own clock.
struct Engine::MicChannel {
  DeviceInfo dev;
  std::string reason;
  std::shared_ptr<VoiceProcessor> vp;
  DriftReader reader{kRate * 2};
  std::unique_ptr<AudioStream> stream;
  std::vector<float> acc;
  int64_t opened_us = 0;
  float saved_gain = -100.f;
  int xfade = -1;  // capture thread only: crossfade position while this channel takes over
  int underruns = 0;  // capture thread only: dry spells seen so far
  const std::function<void(const float *, const float *, int)> *tap = nullptr;

  void on_audio(const float *x, int n) {
    acc.insert(acc.end(), x, x + n);
    size_t at = 0;
    std::array<float, kBlock> raw;
    while (acc.size() - at >= size_t(kBlock)) {
      if (tap) std::copy(acc.begin() + long(at), acc.begin() + long(at) + kBlock, raw.begin());
      vp->process(acc.data() + at);
      if (tap) (*tap)(raw.data(), acc.data() + at, kBlock);
      reader.write(acc.data() + at, kBlock);
      at += kBlock;
    }
    acc.erase(acc.begin(), acc.begin() + long(at));
  }
};

namespace {
constexpr int kXfade = kRate / 50;  // 20 ms crossfade between two microphones

// Where AGC2 starts for a microphone it has not met before: mouth-near
// headset mics are hot, laptop arrays and desk mics quiet.
float default_initial_gain(const DeviceInfo &d) {
  return (d.form == FormFactor::Headset || d.form == FormFactor::Handset) ? 12.f : 22.f;
}

// A render device the mic can hear acoustically.
bool audible_in_room(const DeviceInfo &d) {
  return d.form != FormFactor::Headphones && d.form != FormFactor::Headset && d.form != FormFactor::Handset;
}
}  // namespace

// ---- engine ----------------------------------------------------------------------------
Engine::Engine(Settings s) : settings_(std::move(s)) {
  talk_allowed_ = settings_.talkback;
  max_kbps_ = settings_.max_kbps;
  level_.set_enabled(settings_.auto_level);
  ref2_reader_.set_target(kRate * 30 / 1000);
  talk_mix_.resize(8192);
}

Engine::~Engine() { stop(); }

bool Engine::load_identity(bool fresh) {
  auto path = data_dir() / "host.json";
  std::string s;
  if (!fresh && read_file(path, s)) {
    json j = json::parse(s, nullptr, false);
    if (!j.is_discarded()) {
      std::string c = j.value("code", ""), o = j.value("owner", "");
      // Validate: a truncated or hand-edited file must not produce a code
      // nobody can join.
      if (valid_code(c) && o.size() == 32) { code_ = c; owner_ = o; return true; }
    }
  }
  code_ = generate_code();
  owner_ = random_hex(16);
  write_file_atomic(path, json{{"code", code_}, {"owner", owner_}}.dump());
  LOGI("new join code %s", pretty_code(code_).c_str());
  return true;
}

void Engine::start() {
  if (running_.exchange(true)) return;
  load_identity(settings_.new_code);
  {
    std::lock_guard lk(dev_mu_);
    prefs_ = DevicePrefs::load();
    mic_on_ = settings_.mic && prefs_.mic_on;
  }
  audio_ = create_audio_system();
  LOGI("audio: %s; voice processing: %s", audio_->backend().c_str(),
       VoiceProcessor::apm_available() ? "WebRTC APM (AEC3, NS, AGC2)" : "basic chain (APM not built in)");
  capture_ = audio_->open_system_capture([this](const float *x, int n) { on_system(x, n); });
  event_token_ = audio_->subscribe([this](DeviceEvent, const std::string &) {
    // OS notification thread: just flag it; the device thread does the work.
    std::lock_guard lk(dev_mu_);
    if (!dev_dirty_) dev_dirty_at_ = now_us();
    dev_dirty_ = true;
    dev_cv_.notify_all();
  });
  device_thread_ = std::thread([this] { device_loop(); });
  host_thread_ = std::thread([this] { host_loop(); });
  house_thread_ = std::thread([this] { housekeeping(); });
}

void Engine::stop() {
  if (!running_.exchange(false)) return;
  stopping_ = true;
  cancel_wait_ = true;
  dev_cv_.notify_all();
  if (host_thread_.joinable()) host_thread_.join();
  LOGD("stop: host loop done");
  if (house_thread_.joinable()) house_thread_.join();
  if (device_thread_.joinable()) device_thread_.join();
  if (audio_ && event_token_) audio_->unsubscribe(event_token_);
  for (auto &l : listeners_copy()) { l->close("host stopped"); }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the byes out
  {
    std::lock_guard lk(mu_);
    for (auto &l : listeners_) l->shutdown();
    for (auto &g : graveyard_) g.second->shutdown();
    listeners_.clear();
    graveyard_.clear();
  }
  LOGD("stop: listeners closed");
  if (rdv_) { rdv_->withdraw(); LOGD("stop: withdrawn"); rdv_->stop(); LOGD("stop: rendezvous closed"); }
  capture_.reset();
  close_mic();
  open_output(std::nullopt, std::nullopt);
}

std::vector<std::shared_ptr<Listener>> Engine::listeners_copy() {
  std::lock_guard lk(mu_);
  return listeners_;
}

void Engine::set_status(const std::string &s, const std::string &err) {
  std::lock_guard lk(status_mu_);
  status_ = s;
  error_ = err;
}

// Capture thread, every 10 ms.
void Engine::on_system(const float *x, int frames) {
  auto b = std::make_shared<SourceBlock>();
  memcpy(b->system.data(), x, sizeof(float) * size_t(frames) * 2);
  if (settings_.split) memcpy(b->system_raw.data(), x, sizeof(float) * size_t(frames) * 2);  // before auto level
  level_.process(b->system.data(), frames);
  b->mic_on = mic_on_;
  if (b->mic_on) {
    bool voice = false;
    read_mic(b->voice.data(), &voice);
    b->voice_active = voice;
    mic_active_ = voice;
  }
  meter_.process(b->system.data(), frames, 2);
  std::shared_ptr<const SourceBlock> cb = b;
  std::vector<std::shared_ptr<Listener>> ls;
  {
    std::lock_guard lk(mu_);
    ls = listeners_;
  }
  for (auto &l : ls) l->push(cb);
}

// The processed microphone on the system-capture clock, crossfading to a
// newly chosen device once it has primed, and fading over any gap.
void Engine::read_mic(float *out, bool *voice) {
  std::shared_ptr<MicChannel> cur, nxt;
  {
    std::lock_guard lk(mic_mu_);
    cur = cur_;
    nxt = next_;
  }
  bool ok = cur && cur->reader.read(out, kBlock);
  if (!cur) std::fill(out, out + kBlock, 0.f);
  // The mic ran dry: its device delivers less evenly than we allowed for
  // (Bluetooth mics do, in lumps). Every dry spell is a gap in the voice, a
  // crackle to the listener; so hold 10 ms more each time, up to 150 ms.
  if (cur && cur->reader.underruns() != cur->underruns) {
    cur->underruns = cur->reader.underruns();
    int t = cur->reader.target();
    if (t < kRate * 150 / 1000) {
      cur->reader.set_target(t + kRate / 100);
      LOGD("microphone ran dry (%d times): buffer now %d ms", cur->underruns, (t + kRate / 100) * 1000 / kRate);
    }
  }
  bool v = cur && cur->vp->voice_active();
  if (nxt) {
    if (nxt->xfade < 0 && nxt->reader.fill() >= nxt->reader.target() + kBlock) nxt->xfade = 0;
    if (nxt->xfade >= 0) {
      std::array<float, kBlock> b;
      bool ok2 = nxt->reader.read(b.data(), kBlock);
      // If the old device is already gone, take the new one at full level.
      for (int i = 0; i < kBlock; ++i) {
        float w = ok ? std::min(1.f, float(nxt->xfade + i) / kXfade) : 1.f;
        out[i] = out[i] * (1.f - w) + b[i] * w;
      }
      ok = ok || ok2;
      nxt->xfade += kBlock;
      v = nxt->vp->voice_active();
      if (nxt->xfade >= kXfade) {
        std::lock_guard lk(mic_mu_);
        if (cur_) retired_.push_back(cur_);
        cur_ = next_;
        next_.reset();
        nxt->xfade = -1;
        dev_cv_.notify_all();  // the device thread closes the old stream
      }
    }
  }
  // Fade in when audio (re)starts, out over an underrun: no clicks.
  float target = ok ? 1.f : 0.f;
  float g0 = mic_gain_;
  mic_gain_ = target > g0 ? std::min(1.f, g0 + 0.5f) : std::max(0.f, g0 - 0.5f);
  if (g0 != 1.f || mic_gain_ != 1.f)
    for (int i = 0; i < kBlock; ++i) out[i] *= g0 + (mic_gain_ - g0) * float(i) / kBlock;
  double ss = 0;
  for (int i = 0; i < kBlock; ++i) ss += double(out[i]) * out[i];
  mic_level_db_ = gain_to_db(float(std::sqrt(ss / kBlock)));
  *voice = v;
}

// Render thread: every listener's voice, mixed, with a soft ceiling.
void Engine::render_pull(float *out, int n) {
  std::fill(out, out + n, 0.f);
  if (!talk_allowed_) return;
  float g = db_to_gain(settings_.talk_gain_db);
  for (auto &l : listeners_copy()) l->mix_talk(out, n, g);
  if (settings_.test_talk) settings_.test_talk(out, n);
  for (int i = 0; i < n; ++i) {
    float a = std::fabs(out[i]);
    if (a > 0.7f) out[i] = std::copysign(0.7f + 0.3f * std::tanh((a - 0.7f) / 0.3f), out[i]);
  }
}

void Engine::feed_reference(const float *blk) {
  if (settings_.ref_tap) settings_.ref_tap(blk, kBlock);
  std::shared_ptr<MicChannel> a, b;
  {
    std::lock_guard lk(mic_mu_);
    a = cur_;
    b = next_;
  }
  if (a) a->vp->reverse(blk);
  if (b) b->vp->reverse(blk);
}

// Echo reference thread (loopback of the talkback output): 10 ms blocks to
// every open voice processor, with the default speakers' loopback added
// when the music plays somewhere else.
void Engine::on_reference(const float *x, int n) {
  ref_acc_.insert(ref_acc_.end(), x, x + n);
  size_t at = 0;
  std::array<float, kBlock> blk, sec;
  while (ref_acc_.size() - at >= size_t(kBlock)) {
    std::copy(ref_acc_.begin() + long(at), ref_acc_.begin() + long(at) + kBlock, blk.begin());
    if (ref2_on_ && ref2_reader_.read(sec.data(), kBlock))
      for (int i = 0; i < kBlock; ++i) blk[i] += sec[i];
    feed_reference(blk.data());
    at += kBlock;
  }
  ref_acc_.erase(ref_acc_.begin(), ref_acc_.begin() + long(at));
}

void Engine::on_secondary_reference(const float *x, int n) { ref2_reader_.write(x, n); }

void Engine::set_mic(bool on) {
  {
    std::lock_guard lk(dev_mu_);
    settings_.mic = true;
    prefs_.mic_on = on;
    prefs_.save();
  }
  mic_on_ = on;
  if (!on) {
    close_mic();
    mic_active_ = false;
    mic_level_db_ = -90.f;
  }
  wake_devices();
  for (auto &l : listeners_copy()) l->notify_settings();
}

void Engine::set_talkback(bool on) {
  settings_.talkback = on;
  talk_allowed_ = on;
  wake_devices();
  for (auto &l : listeners_copy()) l->notify_settings();
}

void Engine::set_auto_level(bool on) {
  settings_.auto_level = on;
  level_.set_enabled(on);
}

void Engine::set_max_kbps(int k) {
  settings_.max_kbps = std::clamp(k, 32, 320);
  max_kbps_ = settings_.max_kbps;
}

void Engine::rotate_code() {
  rotate_ = true;
  cancel_wait_ = true;
}

void Engine::kick(int id) {
  for (auto &l : listeners_copy())
    if (l->id() == id) l->close("removed by host");
}

void Engine::housekeeping() {
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<std::shared_ptr<Listener>> to_shut;
    {
      std::lock_guard lk(mu_);
      int64_t now = now_us();
      for (auto it = listeners_.begin(); it != listeners_.end();) {
        if ((*it)->dead()) {
          if ((*it)->was_live()) {
            LOGI("listener %d gone", (*it)->id());
          } else {
            // Signalling worked but no network path formed: both sides are
            // behind NATs that will not talk directly. Their page now
            // retries through the relay on its own.
            ++failed_connects_;
            last_failed_us_ = now;
            refresh_ice_ = true;
            LOGI("listener %d could not connect directly (no network path); their page falls back to the relay",
                 (*it)->id());
          }
          to_shut.push_back(*it);
          graveyard_.push_back({now, *it});
          it = listeners_.erase(it);
        } else ++it;
      }
      // Two attempts can finish at once while one slot is free: keep the cap.
      int live = 0;
      for (auto &l : listeners_) live += l->state() == Listener::State::Live;
      for (auto it = listeners_.rbegin(); live > settings_.max_listeners && it != listeners_.rend(); ++it)
        if ((*it)->state() == Listener::State::Live) { (*it)->close("host full"); --live; }
      // Keep closed peers a few seconds so no callback can drop the last reference.
      graveyard_.erase(std::remove_if(graveyard_.begin(), graveyard_.end(),
                                      [&](auto &g) { return now - g.first > 5000000; }),
                       graveyard_.end());
      int connecting = 0;
      for (auto &l : listeners_) connecting += l->state() != Listener::State::Live;
      listener_count_ = live;
      connecting_count_ = connecting;
    }
    for (auto &l : to_shut) l->shutdown();
  }
}

void Engine::host_loop() {
  std::string host_name = computer_name();
  int backoff = 2000;
  int64_t ice_at = 0;
  std::vector<IceServerCfg> ice;
  if (settings_.offline) {
    set_status("Offline (test)");
    while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return;
  }
  while (running_) {
    if (rotate_ || !rdv_) {
      if (rotate_) {
        for (auto &l : listeners_copy()) l->close("code changed");
        if (rdv_) { rdv_->withdraw(); rdv_->stop(); }
        load_identity(true);
        rotate_ = false;
      }
      published_ = false;
      rdv_ = std::make_shared<Rendezvous>(settings_.service, sha256_hex(code_), owner_, host_name);
      rdv_->start();
      ice_at = 0;
    }
    cancel_wait_ = false;
    int64_t now = now_us();
    // Relay credentials last 24 h; refresh every 30 min (so a relay enabled on
    // the server is picked up without a restart) and right after a listener
    // failed to connect.
    if (!ice_at || now - ice_at > 1800ll * 1000000 || refresh_ice_.exchange(false)) {
      bool turn = false;
      ice = rdv_->ice_servers(&turn);
      if (turn && !turn_) LOGI("relay (TURN) available");
      turn_ = turn;
      ice_at = now;
    }

    // Only connected listeners use up a place. Half-open attempts (a join in
    // progress, or one that will never connect) are capped separately so they
    // can neither block real listeners nor pile up.
    int live = listener_count_;
    if (live >= settings_.max_listeners || connecting_count_ >= 2) {
      std::string err;
      if (rdv_->publish_full(&err)) set_status("Full (" + std::to_string(live) + " listening)");
      for (int i = 0; i < 20 && running_ && !cancel_wait_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    Listener::Env env;
    env.ice = ice;
    env.sim = settings_.sim;
    env.host_name = host_name;
    env.talk_allowed = &talk_allowed_;
    env.mic_on = &mic_on_;
    env.mic_active = &mic_active_;
    env.host_max_kbps = &max_kbps_;
    env.listener_count = &listener_count_;
    env.split = settings_.split;
    int id;
    { std::lock_guard lk(mu_); id = next_id_++; }
    auto l = std::make_shared<Listener>(id, env);
    std::string sdp = l->create_offer(8000, &stopping_);
    if (sdp.empty()) {
      set_status("Network problem", "could not create an offer");
      l->shutdown();
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
      backoff = std::min(backoff * 2, 15000);
      continue;
    }
    std::string session = random_hex(8);
    std::string blob = seal_blob(sdp, code_);
    std::string err;
    if (!rdv_->publish(session, blob, &err)) {
      LOGW("publish failed: %s", err.c_str());
      set_status("Can't reach the server - retrying", err);
      published_ = false;
      l->shutdown();
      for (int i = 0; i < backoff / 100 && running_ && !rotate_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      backoff = std::min(backoff * 2, 15000);
      continue;
    }
    backoff = 2000;
    published_ = true;
    set_status(live ? "Live" : "Ready - waiting for listeners");
    LOGD("published session %s (%zu byte offer)", session.c_str(), blob.size());

    // Refresh an unanswered offer every 4 minutes: NAT bindings behind the
    // server-reflexive candidates expire, and a stale offer is a slow join.
    auto ans = rdv_->wait_answer(session, 240000, cancel_wait_);
    if (!ans) { l->shutdown(); continue; }
    std::string why;
    auto answer_sdp = open_blob(*ans, code_, &why);
    if (!answer_sdp) {
      LOGW("answer rejected: %s", why.c_str());
      l->shutdown();
      continue;
    }
    if (!answer_sdp->empty() && (*answer_sdp)[0] == '{') {
      // The listener's network cannot reach ours directly, so it asked for
      // the relay; its sealed answer carries the relay key instead of an SDP.
      json j = json::parse(*answer_sdp, nullptr, false);
      auto key = j.is_object() && j.value("relay", 0) == 1 ? b64url_decode(j.value("key", "")) : std::nullopt;
      if (!key || key->size() != 32) {
        LOGW("answer rejected: malformed relay request");
        l->shutdown();
        continue;
      }
      std::string url = settings_.service + "/api/room/" + sha256_hex(code_) + "/relay?session=" + session +
                        "&role=host&owner=" + owner_;
      if (!l->start_relay(url, *key, &why, rdv_, session)) {
        LOGW("relay not started: %s", why.c_str());
        l->shutdown();
        continue;
      }
    } else if (!l->set_answer(*answer_sdp, &why)) {
      LOGW("answer not applied: %s", why.c_str());
      l->shutdown();
      continue;
    }
    {
      std::lock_guard lk(mu_);
      listeners_.push_back(l);
      ++connecting_count_;
    }
    LOGI("listener %d joining", id);
    // Loop straight round: the next listener gets a fresh offer immediately.
  }
}


// ---- devices ----------------------------------------------------------------------------
void Engine::wake_devices() {
  {
    std::lock_guard lk(dev_mu_);
    dev_dirty_ = true;
    dev_dirty_at_ = 0;  // no debounce: the owner asked
  }
  dev_cv_.notify_all();
}

std::vector<DeviceInfo> Engine::devices(DeviceKind kind) {
  return audio_ ? audio_->list_devices(kind) : std::vector<DeviceInfo>{};
}

bool Engine::choose_device(DeviceKind kind, const std::string &arg, std::string *message) {
  auto list = devices(kind);
  bool ok;
  {
    std::lock_guard lk(dev_mu_);
    ok = apply_device_choice(prefs_, kind, list, arg, message);
    if (ok) prefs_.save();
    if (ok && kind == DeviceKind::Capture) mic_on_ = settings_.mic && prefs_.mic_on;
  }
  if (!ok) return false;
  LOGI("%s", message ? message->c_str() : "device choice changed");
  if (!mic_on_) close_mic();
  wake_devices();
  for (auto &l : listeners_copy()) l->notify_settings();
  return true;
}

void Engine::device_loop() {
  int64_t last_eval = 0, last_vol = 0, last_save = now_us();
  while (running_) {
    bool eval = false;
    {
      std::unique_lock lk(dev_mu_);
      dev_cv_.wait_for(lk, std::chrono::milliseconds(200));
      if (!running_) break;
      int64_t now = now_us();
      // Debounce bursts of OS notifications (a headset connecting fires a
      // dozen); re-check every 5 s regardless.
      if (dev_dirty_ && now - dev_dirty_at_ > 300000) eval = true;
      if (now - last_eval > 5000000) eval = true;
    }
    std::vector<std::shared_ptr<MicChannel>> old;
    {
      std::lock_guard lk(mic_mu_);
      old.swap(retired_);
    }
    for (auto &ch : old) {
      save_agc(*ch);
      ch->stream.reset();
      LOGD("closed microphone %s", ch->dev.name.c_str());
    }
    if (eval) {
      reevaluate();
      last_eval = now_us();
    }
    int64_t now = now_us();
    std::shared_ptr<MicChannel> cur;
    {
      std::lock_guard lk(mic_mu_);
      cur = cur_;
    }
    if (cur) {
      // The echo canceller's starting delay: our talkback device's buffering
      // plus the microphone's. AEC3 refines it itself.
      double out_ms = 0;
      {
        std::lock_guard lk(out_mu_);
        if (render_) out_ms = render_->status().latency_ms;
      }
      StreamStatus ms = cur->stream->status();
      cur->vp->set_delay_ms(int(std::clamp(out_ms + ms.latency_ms + 10.0, 20.0, 400.0)));
      if (now - last_vol > 5000000) {
        last_vol = now;
        auto v = audio_->input_volume(cur->dev.id);
        std::lock_guard lk(dev_mu_);
        mic_volume_ = v;
      }
      if (now - last_save > 30000000) {
        last_save = now;
        save_agc(*cur);
      }
    }
  }
}

void Engine::reevaluate() {
  auto caps = audio_->list_devices(DeviceKind::Capture);
  auto rens = audio_->list_devices(DeviceKind::Render);
  DevicePrefs p;
  {
    std::lock_guard lk(dev_mu_);
    p = prefs_;
    dev_dirty_ = false;
  }
  DeviceChoice mc = choose_mic(caps, p);
  DeviceChoice sc = choose_speaker(rens, mc.device, p);
  // Music playing on other speakers in the room reaches the mic too: add
  // that device's output to the echo reference.
  std::optional<DeviceInfo> sec;
  for (auto &r : rens)
    if (r.default_console && (!sc.device || r.id != sc.device->id) && audible_in_room(r)) sec = r;
  // Echo exists only when something plays out loud in the room. Earphones,
  // earbuds and headsets: no canceller (it could only eat the owner's voice).
  bool room = (sc.device && audible_in_room(*sc.device)) || sec.has_value();
  if (room != room_echo_.exchange(room))
    LOGI("echo cancellation %s", room ? "on (sound plays on speakers)" : "off (earphones or headset: no echo)");
  bool mic_changed, spk_changed;
  {
    std::lock_guard lk(dev_mu_);
    mic_changed = (mic_choice_.device ? mic_choice_.device->id : "") != (mc.device ? mc.device->id : "") ||
                  mic_choice_.reason != mc.reason;
    spk_changed = (spk_choice_.device ? spk_choice_.device->id : "") != (sc.device ? sc.device->id : "") ||
                  spk_choice_.reason != sc.reason;
    mic_choice_ = mc;
    spk_choice_ = sc;
  }
  if (mic_changed) {
    if (mc.device) {
      LOGI("microphone: %s (%s; %s)", mc.device->name.c_str(), device_type(*mc.device).c_str(), mc.reason.c_str());
      std::string note = bluetooth_note(*mc.device);
      if (!note.empty() && mic_on_) LOGI("%s", note.c_str());
    } else {
      LOGI("microphone: none (%s)", mc.reason.c_str());
    }
  }
  if (spk_changed && sc.device)
    LOGI("talkback output: %s (%s; %s)", sc.device->name.c_str(), device_type(*sc.device).c_str(), sc.reason.c_str());

  {
    std::lock_guard lk(mic_mu_);
    for (auto *ch : {&cur_, &next_}) if (*ch) (*ch)->vp->set_echo_cancel(room);
  }
  if (mic_on_ && mc.device) {
    std::string have;
    {
      std::lock_guard lk(mic_mu_);
      have = next_ ? next_->dev.id : cur_ ? cur_->dev.id : "";
    }
    if (have != mc.device->id) open_mic(*mc.device);
  } else {
    close_mic();
  }
  open_output(sc.device, sec);
}

void Engine::open_mic(const DeviceInfo &d) {
  auto ch = std::make_shared<MicChannel>();
  ch->dev = d;
  VoiceOptions o;
  {
    std::lock_guard lk(dev_mu_);
    auto it = prefs_.agc_gain_db.find(d.id);
    o.initial_gain_db = it != prefs_.agc_gain_db.end() ? it->second : default_initial_gain(d);
    ch->saved_gain = it != prefs_.agc_gain_db.end() ? it->second : -100.f;
  }
  // A headset's mic sits at the mouth with the speakers sealed in the ears:
  // there is no echo worth cancelling, and an echo canceller (or the dip it
  // makes while music plays) can only take the owner's voice away.
  if (d.form == FormFactor::Headset || d.form == FormFactor::Handset) o.aec = o.expander = false;
  ch->vp = std::make_shared<VoiceProcessor>(o);
  ch->vp->set_echo_cancel(room_echo_);
  // A Windows input level far down (7% was seen) leaves the voice so faint
  // that the auto level has to add 30+ dB to it, and with it the noise and
  // every Bluetooth glitch: thin, hissy, crackly. Give the mic its full level;
  // the auto level then only fine-tunes.
  if (auto v = audio_->input_volume(d.id); v && *v < 0.5f) {
    if (audio_->set_input_volume(d.id, 1.f))
      LOGI("raised the Windows input level of %s from %.0f%% to 100%% (too low for a clear voice)", d.name.c_str(), *v * 100);
  }
  // 20 ms absorbs a wired mic's 10 ms event-driven delivery; Bluetooth
  // delivers in lumps from the radio and starts at 50. Both grow on a dry spell.
  ch->reader.set_target(kRate * (d.bus == DeviceBus::Bluetooth ? 50 : 20) / 1000);
  ch->opened_us = now_us();
  if (settings_.mic_tap) ch->tap = &settings_.mic_tap;
  MicChannel *raw = ch.get();
  ch->stream = audio_->open_mic(d.id, [raw](const float *x, int n) { raw->on_audio(x, n); });
  std::lock_guard lk(mic_mu_);
  if (!cur_) {
    cur_ = ch;
  } else {
    if (next_) retired_.push_back(next_);
    next_ = ch;
  }
}

void Engine::close_mic() {
  std::shared_ptr<MicChannel> a, b;
  std::vector<std::shared_ptr<MicChannel>> old;
  {
    std::lock_guard lk(mic_mu_);
    a.swap(cur_);
    b.swap(next_);
    old.swap(retired_);
  }
  for (auto *ch : {&a, &b}) if (*ch) old.push_back(*ch);
  for (auto &ch : old) {
    save_agc(*ch);
    ch->stream.reset();
  }
}

void Engine::save_agc(const MicChannel &ch) {
  VoiceStats st = ch.vp->stats();
  if (!st.agc || st.speech_seconds < 5) return;  // not enough speech to have learned anything
  float g = std::clamp(st.gain_db, 0.f, 30.f);
  std::lock_guard lk(dev_mu_);
  float &slot = prefs_.agc_gain_db[ch.dev.id];
  if (std::fabs(slot - g) < 0.5f) return;
  slot = g;
  prefs_.save();
}

void Engine::open_output(const std::optional<DeviceInfo> &spk, const std::optional<DeviceInfo> &secondary) {
  std::lock_guard lk(out_mu_);
  bool run = running_;
  std::string want = spk ? spk->id : "";
  bool talk = run && talk_allowed_ && spk;
  if (!talk || want != render_id_) { render_.reset(); render_id_.clear(); }
  if (talk && !render_) {
    render_ = audio_->open_render(want, [this](float *x, int n) { render_pull(x, n); });
    render_id_ = want;
  }
  // The echo reference follows the talkback device whether or not talkback
  // is on: the music the owner hears comes out of it too.
  bool need_ref = run && mic_on_ && spk && room_echo_;
  if (!need_ref || want != ref_id_) { ref_loop_.reset(); ref_id_.clear(); ref_name_.clear(); }
  if (need_ref && !ref_loop_) {
    ref_acc_.clear();  // its thread is gone; the next one starts clean
    ref_loop_ = audio_->open_render_loopback(want, [this](const float *x, int n) { on_reference(x, n); });
    ref_id_ = want;
    ref_name_ = spk->name;
  }
  std::string want2 = need_ref && secondary ? secondary->id : "";
  if (want2 != ref2_id_) {
    ref2_on_ = false;
    ref_loop2_.reset();
    ref2_reader_.reset();
    ref2_id_ = want2;
    ref2_name_ = want2.empty() ? "" : secondary->name;
    if (!want2.empty()) {
      ref_loop2_ = audio_->open_render_loopback(want2, [this](const float *x, int n) { on_secondary_reference(x, n); });
      ref2_on_ = true;
      LOGI("echo reference also includes %s (the default output)", ref2_name_.c_str());
    }
  }
}

Snapshot Engine::snapshot() {
  Snapshot s;
  s.code = code_;
  s.pretty = pretty_code(code_);
  s.link = settings_.service + "/" + s.pretty;
  {
    std::lock_guard lk(status_mu_);
    s.status = status_;
    s.error = error_;
  }
  s.published = published_;
  s.socket = rdv_ && rdv_->socket_up();
  s.turn = turn_;
  if (capture_) { s.source = capture_->source(); s.process_loopback = capture_->process_loopback(); }
  s.mic = mic_on_;
  DeviceChoice mc, sc;
  {
    std::lock_guard lk(dev_mu_);
    mc = mic_choice_;
    sc = spk_choice_;
    s.mic_volume = mic_volume_;
  }
  std::shared_ptr<MicChannel> cur, nxt;
  {
    std::lock_guard lk(mic_mu_);
    cur = cur_;
    nxt = next_;
  }
  s.mic_switching = nxt != nullptr;
  DeviceView &m = s.mic_dev;
  m.reason = mc.reason;
  if (mc.device) {
    m.id = mc.device->id;
    m.name = mc.device->name;
    m.type = device_type(*mc.device);
    m.present = true;
    if (mic_on_) m.note = bluetooth_note(*mc.device);
  }
  if (auto ch = nxt ? nxt : cur) {
    StreamStatus st = ch->stream->status();
    m.running = st.running;
    m.error = st.error;
    m.access_denied = st.access_denied;
    m.rate = st.rate;
    m.channels = st.channels;
    m.format = st.format;
    m.latency_ms = st.latency_ms;
  }
  if (cur) s.voice = cur->vp->stats();
  s.mic_name = m.name.empty() ? "no microphone" : m.name;
  s.mic_active = mic_active_;
  s.mic_level_db = mic_level_db_;
  DeviceView &o = s.speaker_dev;
  o.reason = sc.reason;
  if (sc.device) {
    o.id = sc.device->id;
    o.name = sc.device->name;
    o.type = device_type(*sc.device);
    o.present = true;
  }
  {
    std::lock_guard lk(out_mu_);
    if (render_) {
      StreamStatus st = render_->status();
      o.running = st.running;
      o.error = st.error;
      o.rate = st.rate;
      o.channels = st.channels;
      o.latency_ms = st.latency_ms;
    }
    if (ref_loop_) s.echo_reference.push_back(ref_name_);
    if (ref_loop2_) s.echo_reference.push_back(ref2_name_);
  }
  s.talkback = settings_.talkback;
  s.talk_device = o.name;
  s.auto_level = settings_.auto_level;
  s.max_kbps = settings_.max_kbps;
  s.max_listeners = settings_.max_listeners;
  s.split = settings_.split;
  s.failed_connects = failed_connects_;
  s.last_failed_s = last_failed_us_ ? (now_us() - last_failed_us_) / 1e6 : -1;
  s.gain_db = level_.gain_db();
  s.source_lufs = level_.source_lufs();
  meter_.snapshot(s.peak, s.rms);
  for (auto &l : listeners_copy()) s.listeners.push_back(l->view());
  int live = 0;
  for (auto &v : s.listeners) live += v.state == "live";
  if (s.published && live) s.status = std::to_string(live) + (live == 1 ? " listening" : " listening");
  return s;
}

}  // namespace aoi
