// Local control of the background host: the `aoi` commands talk to the
// detached daemon over the platform's IPC channel (platform/ipc.h), one JSON
// request per call.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/audio_system.h"
#include "platform/ipc.h"

namespace aoi {

class Engine;
struct Snapshot;

nlohmann::json snapshot_json(const Snapshot &s);
// A numbered device list, as `aoi mic list` prints it (numbers are 1-based).
nlohmann::json devices_json(const std::vector<DeviceInfo> &list);

class ControlServer {
 public:
  ControlServer(Engine &e, std::function<void()> on_stop) : engine_(e), on_stop_(std::move(on_stop)) {}
  ~ControlServer() { stop(); }
  void start();
  void stop();

 private:
  std::string handle(const std::string &req);
  Engine &engine_;
  std::function<void()> on_stop_;
  std::unique_ptr<IpcServer> ipc_;
};

// Client side. nullopt when no daemon is listening.
std::optional<nlohmann::json> control_call(const nlohmann::json &req, int timeout_ms = 3000);

}  // namespace aoi
