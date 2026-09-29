// Local control of the background host: the `aoi` commands talk to the
// detached daemon over a per-user named pipe, one JSON request per call.
#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace aoi {

class Engine;
struct Snapshot;

std::wstring pipe_name();
nlohmann::json snapshot_json(const Snapshot &s);

class ControlServer {
 public:
  ControlServer(Engine &e, std::function<void()> on_stop) : engine_(e), on_stop_(std::move(on_stop)) {}
  ~ControlServer() { stop(); }
  void start();
  void stop();

 private:
  void run();
  std::string handle(const std::string &req);
  Engine &engine_;
  std::function<void()> on_stop_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  void *stop_event_ = nullptr;
};

// Client side. nullopt when no daemon is listening.
std::optional<nlohmann::json> control_call(const nlohmann::json &req, int timeout_ms = 3000);

}  // namespace aoi
