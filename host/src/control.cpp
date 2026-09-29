#include "control.h"

#include <windows.h>

#include "engine.h"
#include "util.h"

using json = nlohmann::json;

namespace aoi {

std::wstring pipe_name() {
  wchar_t user[256];
  DWORD n = DWORD(std::size(user));
  if (!GetUserNameW(user, &n)) wcscpy(user, L"user");
  return std::wstring(L"\\\\.\\pipe\\aoi-host-") + user;
}

json snapshot_json(const Snapshot &s) {
  json ls = json::array();
  for (const ListenerView &v : s.listeners) {
    ls.push_back({{"id", v.id}, {"name", v.name}, {"state", v.state}, {"path", v.path}, {"rtt_ms", v.rtt_ms},
                  {"loss", v.loss}, {"queue_ms", v.queue_ms}, {"target_kbps", v.target_kbps},
                  {"opus_kbps", v.opus_kbps}, {"wire_kbps", v.wire_kbps}, {"frame_ms", v.frame_ms},
                  {"red", v.red}, {"stereo", v.stereo}, {"fec", v.fec}, {"twcc", v.twcc}, {"nack", v.nack},
                  {"redcap", v.redcap}, {"buffer_ms", v.rx_buffer_ms}, {"conceal", v.rx_conceal},
                  {"retransmits", v.retransmits}, {"packets", v.packets}, {"talking", v.talking},
                  {"talk_level_db", v.talk_level_db}, {"quality", v.quality}, {"cc", v.cc_state},
                  {"why", v.why}, {"connected_s", v.connected_s},
                  {"music_db", v.prefs.music_db}, {"voice_db", v.prefs.voice_db},
                  {"music_mute", v.prefs.music_mute}, {"voice_mute", v.prefs.voice_mute},
                  {"duck", v.prefs.duck}, {"max_kbps", v.prefs.max_kbps}});
  }
  return {{"code", s.code}, {"pretty", s.pretty}, {"link", s.link}, {"status", s.status}, {"error", s.error},
          {"published", s.published}, {"socket", s.socket}, {"turn", s.turn}, {"source", s.source},
          {"process_loopback", s.process_loopback}, {"mic", s.mic}, {"mic_name", s.mic_name},
          {"mic_active", s.mic_active}, {"mic_level_db", s.mic_level_db}, {"talkback", s.talkback},
          {"talk_device", s.talk_device}, {"auto_level", s.auto_level}, {"max_kbps", s.max_kbps},
          {"gain_db", s.gain_db}, {"source_lufs", s.source_lufs}, {"peak", {s.peak[0], s.peak[1]}},
          {"listeners", ls}, {"max_listeners", s.max_listeners}, {"pid", GetCurrentProcessId()},
          {"version", AOI_VERSION}};
}

void ControlServer::start() {
  stop_ = false;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread([this] { run(); });
}

void ControlServer::stop() {
  if (stop_.exchange(true)) return;
  SetEvent(stop_event_);  // wakes the overlapped wait: no racy dummy connection
  if (thread_.joinable()) thread_.join();
  CloseHandle(stop_event_);
}

// Overlapped I/O throughout, so every wait can be interrupted by stop().
static bool wait_io(HANDLE p, OVERLAPPED &ov, HANDLE stop, DWORD *n, DWORD timeout_ms) {
  HANDLE hs[2] = {ov.hEvent, stop};
  DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
  if (w != WAIT_OBJECT_0) {
    CancelIoEx(p, &ov);
    GetOverlappedResult(p, &ov, n, TRUE);
    return false;
  }
  return GetOverlappedResult(p, &ov, n, FALSE) != 0;
}

void ControlServer::run() {
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
        std::string resp = handle(req);
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

std::string ControlServer::handle(const std::string &text) {
  json req = json::parse(text, nullptr, false);
  if (req.is_discarded()) return json{{"ok", false}, {"error", "bad request"}}.dump();
  std::string cmd = req.value("cmd", "");
  json out = {{"ok", true}};
  if (cmd == "status") {
    out["status"] = snapshot_json(engine_.snapshot());
  } else if (cmd == "stop") {
    LOGI("stop requested");
    std::thread([cb = on_stop_] { Sleep(100); cb(); }).detach();
  } else if (cmd == "new-code") {
    engine_.rotate_code();
  } else if (cmd == "kick") {
    engine_.kick(req.value("id", 0));
  } else {
    out = {{"ok", false}, {"error", "unknown command"}};
  }
  return out.dump();
}

std::optional<json> control_call(const json &req, int timeout_ms) {
  std::string s = req.dump();
  std::string resp(1 << 16, '\0');
  DWORD got = 0;
  if (!CallNamedPipeW(pipe_name().c_str(), s.data(), DWORD(s.size()), resp.data(), DWORD(resp.size()), &got,
                      DWORD(timeout_ms)))
    return std::nullopt;
  resp.resize(got);
  json j = json::parse(resp, nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  return j;
}

}  // namespace aoi
