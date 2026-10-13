// platform/http.h, ipc.h and installer.h on macOS: not supported yet (see
// README.md: NSURLSession, a UNIX domain socket, a copy to ~/Applications).
#include "platform/http.h"
#include "platform/installer.h"
#include "platform/ipc.h"
#include "platform/os.h"

namespace aoi {

namespace {
const char *kNotYet = "not supported on macOS yet";
}

// ---- http ------------------------------------------------------------------------
struct Http::Impl {};
Http::Http() : impl_(std::make_unique<Impl>()) {}
Http::~Http() = default;

HttpResult Http::request(const std::string &, const std::string &, const std::string &, int) {
  HttpResult r;
  r.error = kNotYet;
  return r;
}

std::unique_ptr<WebSocket> Http::websocket(const std::string &, int, std::string *err) {
  if (err) *err = kNotYet;
  return nullptr;
}

// ---- ipc -------------------------------------------------------------------------
namespace {
class NoIpc final : public IpcServer {
 public:
  void start(Handler) override {}
  void stop() override {}
};
}  // namespace

std::unique_ptr<IpcServer> create_ipc_server() { return std::make_unique<NoIpc>(); }
std::optional<std::string> ipc_call(const std::string &, int) { return std::nullopt; }

// ---- installer -------------------------------------------------------------------
std::filesystem::path install_dir() { return data_dir() / "bin"; }
std::filesystem::path installed_exe() { return install_dir() / "aoi"; }
bool is_installed_copy() { return false; }
bool install(std::string *err) {
  if (err) *err = kNotYet;
  return false;
}
void cleanup_old_copies() {}
bool uninstall(bool, std::string *err) {
  if (err) *err = kNotYet;
  return false;
}
bool autostart_enabled() { return false; }
void set_autostart(bool) {}
std::string release_asset_name() { return "aoi-macos"; }

}  // namespace aoi
