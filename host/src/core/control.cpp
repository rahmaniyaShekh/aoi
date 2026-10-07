#include "control.h"

#include <thread>

#include "engine.h"
#include "util.h"

using json = nlohmann::json;

namespace aoi {

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
          {"listeners", ls}, {"max_listeners", s.max_listeners},
          {"failed_connects", s.failed_connects}, {"last_failed_s", s.last_failed_s}, {"pid", current_pid()},
          {"version", AOI_VERSION}};
}

void ControlServer::start() {
  ipc_ = create_ipc_server();
  ipc_->start([this](const std::string &req) { return handle(req); });
}

void ControlServer::stop() {
  if (ipc_) ipc_->stop();
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
    std::thread([cb = on_stop_] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cb(); }).detach();
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
  auto resp = ipc_call(req.dump(), timeout_ms);
  if (!resp) return std::nullopt;
  json j = json::parse(*resp, nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  return j;
}

}  // namespace aoi
