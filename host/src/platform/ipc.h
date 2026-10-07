// Local control channel between the `aoi` command and the background host:
// one request, one response, both text (JSON), reachable only by this user.
// Windows: a per-user named pipe.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace aoi {

class IpcServer {
 public:
  using Handler = std::function<std::string(const std::string &request)>;
  virtual ~IpcServer() = default;
  virtual void start(Handler h) = 0;
  virtual void stop() = 0;  // returns promptly, even mid-request
};

std::unique_ptr<IpcServer> create_ipc_server();

// nullopt when no host is listening (or it did not answer in time).
std::optional<std::string> ipc_call(const std::string &request, int timeout_ms);

}  // namespace aoi
