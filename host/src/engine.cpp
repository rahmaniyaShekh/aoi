#include "engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

#include "crypto.h"
#include "util.h"

using json = nlohmann::json;

namespace aoi {

// ---- engine ----------------------------------------------------------------------------
Engine::Engine(Settings s) : settings_(std::move(s)) {
  talk_allowed_ = settings_.talkback;
  mic_on_ = settings_.mic;
  max_kbps_ = settings_.max_kbps;
  level_.set_enabled(settings_.auto_level);
  mic_reader_.set_target(kRate * 30 / 1000);  // 30 ms head start absorbs the mic's delivery jitter
  talk_mix_.resize(8192);
}

Engine::~Engine() { stop(); }

bool Engine::load_identity(bool fresh) {
  auto path = data_dir() / L"host.json";
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
  capture_ = std::make_unique<SystemCapture>([this](const float *x, int n) { on_system(x, n); });
  capture_->start();
  if (settings_.mic) set_mic(true);
  render_ = std::make_unique<Render>([this](float *x, int n) { render_pull(x, n); });
  if (settings_.talkback) render_->start();
  host_thread_ = std::thread([this] { host_loop(); });
  house_thread_ = std::thread([this] { housekeeping(); });
}

void Engine::stop() {
  if (!running_.exchange(false)) return;
  stopping_ = true;
  cancel_wait_ = true;
  if (host_thread_.joinable()) host_thread_.join();
  LOGD("stop: host loop done");
  if (house_thread_.joinable()) house_thread_.join();
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
  if (capture_) capture_->stop();
  if (mic_) mic_->stop();
  if (render_) render_->stop();
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
  level_.process(b->system.data(), frames);
  b->mic_on = mic_on_;
  if (b->mic_on) {
    mic_reader_.read(b->voice.data(), frames);
    voice_.process(b->voice.data(), frames);
    b->voice_active = voice_.active();
    mic_level_db_ = voice_.level_db();
    mic_active_ = b->voice_active;
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

// Render thread: every listener's voice, mixed, with a soft ceiling.
void Engine::render_pull(float *out, int n) {
  std::fill(out, out + n, 0.f);
  if (!talk_allowed_) return;
  float g = db_to_gain(settings_.talk_gain_db);
  for (auto &l : listeners_copy()) l->mix_talk(out, n, g);
  for (int i = 0; i < n; ++i) {
    float a = std::fabs(out[i]);
    if (a > 0.7f) out[i] = std::copysign(0.7f + 0.3f * std::tanh((a - 0.7f) / 0.3f), out[i]);
  }
}

void Engine::set_mic(bool on) {
  settings_.mic = on;
  mic_on_ = on;
  if (on) {
    if (!mic_) {
      mic_reader_.reset();
      mic_ = std::make_unique<MicCapture>([this](const float *x, int n) { mic_reader_.write(x, n); });
      mic_->start();
    }
  } else if (mic_) {
    mic_->stop();
    mic_.reset();
    mic_active_ = false;
    mic_level_db_ = -90.f;
  }
  for (auto &l : listeners_copy()) l->notify_settings();
}

void Engine::set_talkback(bool on) {
  settings_.talkback = on;
  talk_allowed_ = on;
  if (render_) {
    if (on && !render_->running()) render_->start();
    if (!on) render_->stop();
  }
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
  while (running_) {
    if (rotate_ || !rdv_) {
      if (rotate_) {
        for (auto &l : listeners_copy()) l->close("code changed");
        if (rdv_) { rdv_->withdraw(); rdv_->stop(); }
        load_identity(true);
        rotate_ = false;
      }
      published_ = false;
      rdv_ = std::make_unique<Rendezvous>(settings_.service, sha256_hex(code_), owner_, host_name);
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
      if (!l->start_relay(url, *key, &why)) {
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
  s.mic = settings_.mic;
  s.mic_name = mic_ ? mic_->name() : "";
  s.mic_active = mic_active_;
  s.mic_level_db = mic_level_db_;
  s.talkback = settings_.talkback;
  s.talk_device = render_ ? render_->name() : "";
  s.auto_level = settings_.auto_level;
  s.max_kbps = settings_.max_kbps;
  s.max_listeners = settings_.max_listeners;
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
