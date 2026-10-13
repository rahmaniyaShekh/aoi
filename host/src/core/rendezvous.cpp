#include "rendezvous.h"

#include <nlohmann/json.hpp>

#include "util.h"

using json = nlohmann::json;

namespace aoi {

Rendezvous::Rendezvous(std::string base, std::string room_id, std::string owner, std::string name)
    : base_(std::move(base)), room_(std::move(room_id)), owner_(std::move(owner)), name_(std::move(name)) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

Rendezvous::~Rendezvous() { stop(); }

void Rendezvous::start() {
  stop_ = false;
  thread_ = std::thread([this] { socket_loop(); });
  keepalive_ = std::thread([this] {
    // Answered by the Durable Object runtime without waking it: costs nothing,
    // and keeps NATs, proxies and Cloudflare's idle timer from closing the socket.
    while (!stop_) {
      std::unique_lock lk(mu_);
      cv_.wait_for(lk, std::chrono::seconds(25), [&] { return stop_.load(); });
      lk.unlock();
      if (stop_) break;
      if (ws_up_) send_json("ping");
    }
  });
}

void Rendezvous::stop() {
  if (stop_.exchange(true)) return;
  // Close outside the lock: the socket thread needs mu_ to finish, and
  // closing waits for its pending receive.
  std::shared_ptr<WebSocket> ws;
  {
    std::lock_guard lk(mu_);
    ws = ws_;
  }
  if (ws) ws->close();
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (keepalive_.joinable()) keepalive_.join();
}

bool Rendezvous::send_json(const std::string &s) {
  std::shared_ptr<WebSocket> ws;
  {
    std::lock_guard lk(mu_);
    ws = ws_;
  }
  return ws && ws->send(s);  // outside the lock: a send can block on a slow uplink
}

bool Rendezvous::relay_join(const std::string &session, RelaySub sub, int timeout_ms) {
  {
    std::lock_guard lk(mu_);
    relays_[session] = std::move(sub);
    relay_ok_.erase(session);
  }
  if (!ws_up_ || !send_json(json{{"t", "rj"}, {"session", session}}.dump())) { relay_leave(session); return false; }
  std::unique_lock lk(mu_);
  if (cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return relay_ok_.count(session) || stop_; }) &&
      relay_ok_.count(session))
    return true;
  lk.unlock();
  relay_leave(session);
  return false;
}

bool Rendezvous::relay_send(const std::string &session, const uint8_t *p, size_t n) {
  std::shared_ptr<WebSocket> ws;
  {
    std::lock_guard lk(mu_);
    ws = ws_;
  }
  if (!ws || !ws_up_) return false;
  std::string out;
  out.reserve(16 + n);
  out.append(session);
  out.append(reinterpret_cast<const char *>(p), n);
  return ws->send_binary(out.data(), out.size());
}

void Rendezvous::relay_leave(const std::string &session) {
  bool had;
  {
    std::lock_guard lk(mu_);
    had = relays_.erase(session) > 0;
    relay_ok_.erase(session);
  }
  if (had && ws_up_) send_json(json{{"t", "rx"}, {"session", session}}.dump());
}

void Rendezvous::socket_loop() {
  // WinHTTP performs the upgrade on an https:// request.
  std::string upgrade = base_ + "/api/room/" + room_ + "/host?owner=" + owner_;
  // Short attempts and short waits between them: on a network that drops
  // some new connections, the next attempt usually lands at once, and every
  // second without the socket is a second a listener waits.
  int backoff = 500;
  while (!stop_) {
    std::string err;
    std::shared_ptr<WebSocket> ws = http_.websocket(upgrade, 5000, &err);
    if (!ws) {
      {
        std::lock_guard lk(mu_);
        last_error_ = "server socket: " + err;
        ++ws_failures_;
      }
      LOGW("rendezvous socket failed: %s (falling back to polling)", err.c_str());
      std::unique_lock lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(backoff), [&] { return stop_.load(); });
      backoff = std::min(backoff * 2, 8000);
      continue;
    }
    std::shared_ptr<WebSocket> raw = ws;
    std::string session;
    std::vector<std::string> rejoin;
    {
      std::lock_guard lk(mu_);
      ws_ = std::move(ws);
      ws_failures_ = 0;
      last_error_.clear();
      session = current_session_;
      for (auto &kv : relays_) rejoin.push_back(kv.first);
    }
    ws_up_ = true;
    backoff = 500;
    LOGI("rendezvous socket connected");
    // Anything pushed while we were away is still stored: ask for it.
    if (!session.empty()) send_json(json{{"t", "get"}, {"session", session}}.dump());
    // Relays that were running over the old socket carry on over this one.
    for (auto &s : rejoin) send_json(json{{"t", "rj"}, {"session", s}}.dump());
    cv_.notify_all();

    while (!stop_) {
      bool bin = false;
      auto msg = raw->receive(&bin);
      if (!msg) break;
      if (bin) {
        // Relay traffic: [16-character session][the listener's message].
        if (msg->size() <= 16) continue;
        std::function<void(std::string &&)> fn;
        {
          std::lock_guard lk(mu_);
          auto it = relays_.find(msg->substr(0, 16));
          if (it != relays_.end()) fn = it->second.on_msg;
        }
        if (fn) fn(msg->substr(16));
        continue;
      }
      if (*msg == "pong") continue;
      json m = json::parse(*msg, nullptr, false);
      if (m.is_discarded() || !m.is_object()) continue;
      std::string t = m.value("t", "");
      if (t == "rc") {  // the listener's side of a relay closed
        std::function<void()> fn;
        {
          std::lock_guard lk(mu_);
          auto it = relays_.find(m.value("session", ""));
          if (it != relays_.end()) fn = it->second.on_closed;
        }
        if (fn) fn();
        continue;
      }
      std::lock_guard lk(mu_);
      if (t == "ans") {
        answers_[m.value("session", "")] = m.value("answer", "");
      } else if (t == "ok" || t == "err") {
        replies_[m.value("rid", 0)] = t == "ok" ? "ok" : m.value("error", "error");
      } else if (t == "rj") {
        relay_ok_.insert(m.value("session", ""));
      }
      cv_.notify_all();
    }
    ws_up_ = false;
    {
      std::lock_guard lk(mu_);
      ws_.reset();
    }
    cv_.notify_all();
    if (!stop_) LOGW("rendezvous socket closed; reconnecting");
  }
}

bool Rendezvous::http_publish(const std::string &body, std::string *err) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto r = http_.request("POST", base_ + "/api/room", body, 12000);
    if (r.status == 201) return true;
    std::string e = r.status ? ("HTTP " + std::to_string(r.status) + " " + r.body) : r.error;
    if (err) *err = e;
    if (r.status == 403 || r.status == 400) return false;  // retrying cannot help
    std::this_thread::sleep_for(std::chrono::milliseconds(1000 * (attempt + 1)));
  }
  return false;
}

bool Rendezvous::publish(const std::string &session, const std::string &blob, std::string *err) {
  {
    std::lock_guard lk(mu_);
    current_session_ = session;
    answers_.erase(session);
  }
  // Give a just-started socket a moment before settling for HTTP.
  {
    std::unique_lock lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(ws_failures_ ? 0 : 3000), [&] { return ws_up_.load() || stop_; });
  }
  if (ws_up_) {
    int rid;
    { std::lock_guard lk(mu_); rid = next_rid_++; }
    json m = {{"t", "pub"}, {"session", session}, {"offer", blob}, {"name", name_}, {"caps", {"relay"}}, {"rid", rid}};
    if (send_json(m.dump())) {
      std::unique_lock lk(mu_);
      if (cv_.wait_for(lk, std::chrono::seconds(8), [&] { return replies_.count(rid) || !ws_up_ || stop_; }) &&
          replies_.count(rid)) {
        std::string r = replies_[rid];
        replies_.erase(rid);
        if (r == "ok") return true;
        if (err) *err = r;
        LOGW("publish over socket refused: %s", r.c_str());
        if (r == "forbidden") return false;
      }
    }
  }
  json b = {{"id", room_}, {"session", session}, {"offer", blob}, {"owner", owner_}, {"name", name_}, {"caps", {"relay"}}};
  return http_publish(b.dump(), err);
}

bool Rendezvous::publish_full(std::string *err) {
  {
    std::lock_guard lk(mu_);
    current_session_.clear();
  }
  if (ws_up_) {
    int rid;
    { std::lock_guard lk(mu_); rid = next_rid_++; }
    if (send_json(json{{"t", "pub"}, {"full", true}, {"name", name_}, {"rid", rid}}.dump())) {
      std::unique_lock lk(mu_);
      if (cv_.wait_for(lk, std::chrono::seconds(8), [&] { return replies_.count(rid) || !ws_up_ || stop_; }) &&
          replies_.count(rid) && replies_[rid] == "ok") {
        replies_.erase(rid);
        return true;
      }
    }
  }
  json b = {{"id", room_}, {"full", true}, {"owner", owner_}, {"name", name_}};
  return http_publish(b.dump(), err);
}

std::optional<std::string> Rendezvous::wait_answer(const std::string &session, int timeout_ms,
                                                    const std::atomic<bool> &cancel) {
  const int64_t deadline = now_us() + int64_t(timeout_ms) * 1000;
  int64_t next_poll = 0, next_get = now_us() + 20000000;
  while (!stop_ && !cancel && now_us() < deadline) {
    {
      std::unique_lock lk(mu_);
      auto it = answers_.find(session);
      if (it != answers_.end()) {
        std::string a = it->second;
        answers_.erase(it);
        lk.unlock();
        send_json(json{{"t", "ack"}, {"session", session}}.dump());
        return a;
      }
      cv_.wait_for(lk, std::chrono::milliseconds(250));
    }
    int64_t now = now_us();
    if (!ws_up_ && now >= next_poll) {
      // Fallback mailbox polling: 2 s, which also keeps the room alive.
      next_poll = now + 2000000;
      auto r = http_.request("GET", base_ + "/api/room/" + room_ + "/answer?session=" + session, {}, 10000);
      if (r.status == 200) {
        json j = json::parse(r.body, nullptr, false);
        if (!j.is_discarded() && j.contains("answer")) return j["answer"].get<std::string>();
      }
    } else if (ws_up_ && now >= next_get) {
      // Belt and braces: a push lost in a half-open socket is fetched anyway.
      next_get = now + 20000000;
      send_json(json{{"t", "get"}, {"session", session}}.dump());
    }
  }
  return std::nullopt;
}

void Rendezvous::withdraw() {
  if (ws_up_) send_json(json{{"t", "del"}, {"rid", 0}}.dump());
  http_.request("DELETE", base_ + "/api/room/" + room_ + "?owner=" + owner_, {}, 4000);
}

std::vector<IceServerCfg> Rendezvous::ice_servers(bool *turn) {
  std::vector<IceServerCfg> out;
  auto r = http_.request("GET", base_ + "/api/turn", {}, 5000);
  bool have_turn = false;
  if (r.status == 200) {
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_discarded() && j.contains("iceServers")) {
      have_turn = j.value("turn", false);
      for (auto &s : j["iceServers"]) {
        std::vector<std::string> urls;
        if (s["urls"].is_string()) urls.push_back(s["urls"]);
        else for (auto &u : s["urls"]) urls.push_back(u);
        for (auto &u : urls)
          out.push_back({u, s.value("username", ""), s.value("credential", "")});
      }
    }
  }
  if (out.empty()) {
    out.push_back({"stun:stun.cloudflare.com:3478", "", ""});
    out.push_back({"stun:stun.l.google.com:19302", "", ""});
  }
  if (turn) *turn = have_turn;
  return out;
}

}  // namespace aoi
