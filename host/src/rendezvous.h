// Client for the AOI rendezvous (Cloudflare Worker + Durable Object).
//
// Preferred transport is a hibernating WebSocket: publishes go up it and
// answers are pushed down it the instant a listener posts one. If WebSockets
// are broken on this network (some proxies), it falls back to the plain HTTP
// mailbox API and polls, exactly as the reference pattern does.
#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "http.h"

namespace aoi {

struct IceServerCfg {
  std::string url;       // stun:host:port / turn:host:port?transport=udp / turns:...
  std::string username, credential;
};

class Rendezvous {
 public:
  Rendezvous(std::string base, std::string room_id, std::string owner, std::string name);
  ~Rendezvous();

  void start();  // connects the host socket in the background
  void stop();

  // Publish an offer (or "full"). Retries internally; false after ~3 attempts.
  bool publish(const std::string &session, const std::string &blob, std::string *err = nullptr);
  bool publish_full(std::string *err = nullptr);
  // Wait for the answer to `session`. Returns nullopt on timeout or cancel.
  std::optional<std::string> wait_answer(const std::string &session, int timeout_ms, const std::atomic<bool> &cancel);
  void withdraw();  // DELETE the room (clean shutdown)

  std::vector<IceServerCfg> ice_servers(bool *turn = nullptr);
  bool socket_up() const { return ws_up_; }
  std::string last_error() const { std::lock_guard lk(mu_); return last_error_; }
  const std::string &base() const { return base_; }

 private:
  void socket_loop();
  bool send_json(const std::string &s);
  bool http_publish(const std::string &body, std::string *err);

  std::string base_, room_, owner_, name_;
  Http http_;
  std::thread thread_, keepalive_;
  std::atomic<bool> stop_{false}, ws_up_{false};
  std::unique_ptr<WebSocket> ws_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::string> answers_;   // session -> sealed answer
  std::map<int, std::string> replies_;           // rid -> "ok" / error
  int next_rid_ = 1;
  std::string current_session_;
  std::string last_error_;
  int ws_failures_ = 0;
};

}  // namespace aoi
