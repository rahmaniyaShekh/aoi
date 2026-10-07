// platform/ipc.h on Windows: a per-user named pipe, one message each way.
#include "platform/ipc.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "platform/os.h"

namespace aoi {

namespace {

std::wstring pipe_name() {
  wchar_t user[256];
  DWORD n = DWORD(std::size(user));
  if (!GetUserNameW(user, &n)) wcscpy(user, L"user");
  return std::wstring(L"\\\\.\\pipe\\aoi-host-") + user;
}

// Overlapped I/O throughout, so every wait can be interrupted by stop().
bool wait_io(HANDLE p, OVERLAPPED &ov, HANDLE stop, DWORD *n, DWORD timeout_ms) {
  HANDLE hs[2] = {ov.hEvent, stop};
  DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
  if (w != WAIT_OBJECT_0) {
    CancelIoEx(p, &ov);
    GetOverlappedResult(p, &ov, n, TRUE);
    return false;
  }
  return GetOverlappedResult(p, &ov, n, FALSE) != 0;
}

class PipeServer final : public IpcServer {
 public:
  ~PipeServer() override { stop(); }
  void start(Handler h) override {
    handler_ = std::move(h);
    stop_ = false;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
  }
  void stop() override {
    if (!stop_event_ || stop_.exchange(true)) return;
    SetEvent(stop_event_);  // wakes the overlapped wait: no racy dummy connection
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }

 private:
  void run() {
    std::wstring name = pipe_name();
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!stop_) {
      // Default security: only this user (and SYSTEM/admins) can connect.
      HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                  PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
      if (p == INVALID_HANDLE_VALUE) { WaitForSingleObject(stop_event_, 500); continue; }
      OVERLAPPED ov{};
      ov.hEvent = ev;
      ResetEvent(ev);
      DWORD n = 0;
      bool connected = ConnectNamedPipe(p, &ov) != 0;
      if (!connected) {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) connected = true;
        else if (e == ERROR_IO_PENDING) connected = wait_io(p, ov, stop_event_, &n, INFINITE);
      }
      if (connected && !stop_) {
        std::string req(1 << 16, '\0');
        ResetEvent(ev);
        bool ok = ReadFile(p, req.data(), DWORD(req.size()), &n, &ov) != 0;
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = wait_io(p, ov, stop_event_, &n, 3000);
        if (ok && n) {
          req.resize(n);
          std::string resp = handler_(req);
          ResetEvent(ev);
          bool w = WriteFile(p, resp.data(), DWORD(resp.size()), &n, &ov) != 0;
          if (!w && GetLastError() == ERROR_IO_PENDING) wait_io(p, ov, stop_event_, &n, 3000);
        }
      }
      DisconnectNamedPipe(p);
      CloseHandle(p);
    }
    CloseHandle(ev);
  }

  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  HANDLE stop_event_ = nullptr;
};

}  // namespace

std::unique_ptr<IpcServer> create_ipc_server() { return std::make_unique<PipeServer>(); }

std::optional<std::string> ipc_call(const std::string &s, int timeout_ms) {
  std::string resp(1 << 16, '\0');
  DWORD got = 0;
  if (!CallNamedPipeW(pipe_name().c_str(), const_cast<char *>(s.data()), DWORD(s.size()), resp.data(),
                      DWORD(resp.size()), &got, DWORD(timeout_ms)))
    return std::nullopt;
  resp.resize(got);
  return resp;
}

}  // namespace aoi
