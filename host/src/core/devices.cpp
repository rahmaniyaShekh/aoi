#include "devices.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "util.h"

using json = nlohmann::json;

namespace aoi {

namespace {

bool is_headset(const DeviceInfo &d) { return d.form == FormFactor::Headset || d.form == FormFactor::Handset; }

// Among several equally good devices, the one Windows itself would use.
const DeviceInfo *best_of(const std::vector<const DeviceInfo *> &c) {
  if (c.empty()) return nullptr;
  for (auto *d : c) if (d->default_comms) return d;
  for (auto *d : c) if (d->default_console) return d;
  return c.front();
}

const DeviceInfo *find_explicit(const std::vector<DeviceInfo> &list, const std::string &id, const std::string &name) {
  for (auto &d : list) if (d.id == id) return &d;
  // Endpoint ids survive reconnects; a re-paired headset gets a new one but keeps its name.
  if (!name.empty())
    for (auto &d : list) if (d.name == name) return &d;
  return nullptr;
}

// "Headset (CMF Buds 2a)" -> "CMF Buds 2a"
std::string short_name(const std::string &n) {
  auto a = n.find('('), b = n.rfind(')');
  if (a != std::string::npos && b != std::string::npos && b > a + 1) return n.substr(a + 1, b - a - 1);
  return n;
}

}  // namespace

const char *to_string(DeviceBus b) {
  switch (b) {
    case DeviceBus::BuiltIn: return "built-in";
    case DeviceBus::Usb: return "USB";
    case DeviceBus::Bluetooth: return "Bluetooth";
    case DeviceBus::Hdmi: return "HDMI";
    case DeviceBus::Virtual: return "virtual";
    default: return "unknown";
  }
}

const char *to_string(FormFactor f) {
  switch (f) {
    case FormFactor::Speakers: return "speakers";
    case FormFactor::LineLevel: return "line";
    case FormFactor::Headphones: return "headphones";
    case FormFactor::Microphone: return "microphone";
    case FormFactor::Headset: return "headset";
    case FormFactor::Handset: return "handset";
    case FormFactor::Digital: return "digital";
    case FormFactor::Hdmi: return "display";
    case FormFactor::Network: return "network";
    default: return "audio device";
  }
}

std::string device_type(const DeviceInfo &d) {
  const char *form = to_string(d.form);
  switch (d.bus) {
    case DeviceBus::Bluetooth:
      if (is_headset(d)) return d.bluetooth_le ? "Bluetooth LE headset" : "Bluetooth headset";
      return std::string("Bluetooth ") + (d.form == FormFactor::Unknown ? "audio" : form);
    case DeviceBus::Usb: return std::string("USB ") + (d.form == FormFactor::Unknown ? "audio" : form);
    case DeviceBus::BuiltIn:
      if (d.form == FormFactor::Headset) return "wired headset";
      if (d.form == FormFactor::Headphones) return "headphone jack";
      return std::string("built-in ") + form;
    case DeviceBus::Hdmi: return "HDMI / display";
    case DeviceBus::Virtual: return std::string("virtual ") + form;
    default: return form;
  }
}

DeviceChoice choose_mic(const std::vector<DeviceInfo> &caps, const DevicePrefs &prefs) {
  DeviceChoice c;
  std::string prefix;
  if (!prefs.mic_id.empty() || !prefs.mic_name.empty()) {
    if (auto *d = find_explicit(caps, prefs.mic_id, prefs.mic_name)) {
      c.device = *d;
      c.reason = "your choice";
      return c;
    }
    c.chosen_missing = true;
    prefix = "your choice (" + (prefs.mic_name.empty() ? std::string("a microphone") : prefs.mic_name) +
             ") is not connected; ";
  }
  // A headset mic is the best mic: close to the mouth, and its own earpieces
  // cannot leak into it. Except a classic Bluetooth one: opening it switches
  // the headset to the hands-free profile, so the owner's own music drops to
  // call quality (and some players stop when their output changes under
  // them). That one is used only when chosen, or when it is the only mic.
  std::vector<const DeviceInfo *> wired, le, hfp, other;
  for (auto &d : caps) {
    if (!is_headset(d)) other.push_back(&d);
    else if (d.bus != DeviceBus::Bluetooth) wired.push_back(&d);
    else if (d.bluetooth_hfp) hfp.push_back(&d);
    else le.push_back(&d);
  }
  const DeviceInfo *pick = nullptr;
  std::string why;
  if ((pick = best_of(wired))) why = pick->bus == DeviceBus::Usb ? "USB headset" : "wired headset";
  else if ((pick = best_of(le))) why = "Bluetooth LE headset";
  else {
    for (auto *d : other) if (d->default_console) { pick = d; why = "Windows default"; break; }
    if (!pick) for (auto *d : other) if (d->default_comms) { pick = d; why = "Windows communications device"; break; }
    if (!pick && !other.empty()) { pick = other.front(); why = "only microphone"; }
    if (!pick && (pick = best_of(hfp))) why = "Bluetooth headset, the only microphone";
  }
  if (!pick) {
    c.reason = prefix + "no microphone";
    return c;
  }
  c.device = *pick;
  c.reason = prefix + "auto: " + why;
  return c;
}

DeviceChoice choose_speaker(const std::vector<DeviceInfo> &renders, const std::optional<DeviceInfo> &mic,
                            const DevicePrefs &prefs) {
  DeviceChoice c;
  std::string prefix;
  if (!prefs.speaker_id.empty() || !prefs.speaker_name.empty()) {
    if (auto *d = find_explicit(renders, prefs.speaker_id, prefs.speaker_name)) {
      c.device = *d;
      c.reason = "your choice";
      return c;
    }
    c.chosen_missing = true;
    prefix = "your choice (" + (prefs.speaker_name.empty() ? std::string("an output") : prefs.speaker_name) +
             ") is not connected; ";
  }
  const DeviceInfo *pick = nullptr;
  std::string why;
  // Same physical device as the mic: a headset's own earpieces. Not for the
  // PC's built-in mic, whose "container" is the whole PC: the owner may be
  // wearing headphones while using the laptop mic.
  if (mic && !mic->container.empty() && !mic->builtin_container) {
    std::vector<const DeviceInfo *> same;
    for (auto &d : renders) if (d.container == mic->container) same.push_back(&d);
    // Older Windows builds expose a separate hands-free output: prefer the
    // one that matches the mic's profile, since that is the link in use.
    for (auto *d : same) if (d->bluetooth_hfp == mic->bluetooth_hfp) { pick = d; break; }
    if (!pick && !same.empty()) pick = best_of(same);
    if (pick) why = "same device as the mic";
  }
  // Otherwise where the owner is listening: Windows' default output.
  if (!pick) for (auto &d : renders) if (d.default_console) { pick = &d; why = "Windows default"; break; }
  if (!pick) for (auto &d : renders) if (d.default_comms) { pick = &d; why = "Windows communications device"; break; }
  if (!pick && !renders.empty()) { pick = &renders.front(); why = "only output"; }
  if (!pick) {
    c.reason = prefix + "no output device";
    return c;
  }
  c.device = *pick;
  c.reason = prefix + "auto: " + why;
  return c;
}

bool apply_device_choice(DevicePrefs &prefs, DeviceKind kind, const std::vector<DeviceInfo> &list,
                         const std::string &arg_in, std::string *message) {
  bool mic = kind == DeviceKind::Capture;
  const char *what = mic ? "Microphone" : "Talkback output";
  std::string arg = lower_ascii(arg_in);
  auto set = [&](const DeviceInfo &d) {
    (mic ? prefs.mic_id : prefs.speaker_id) = d.id;
    (mic ? prefs.mic_name : prefs.speaker_name) = d.name;
    if (mic) prefs.mic_on = true;
    if (message) *message = std::string(what) + ": " + d.name + " (" + device_type(d) + ")";
    return true;
  };
  if (arg == "auto") {
    (mic ? prefs.mic_id : prefs.speaker_id).clear();
    (mic ? prefs.mic_name : prefs.speaker_name).clear();
    if (mic) prefs.mic_on = true;
    if (message) *message = std::string(what) + ": automatic";
    return true;
  }
  if (mic && (arg == "off" || arg == "on")) {
    prefs.mic_on = arg == "on";
    if (message) *message = std::string("Microphone ") + (prefs.mic_on ? "on" : "off");
    return true;
  }
  if (arg == "laptop" || arg == "builtin" || arg == "built-in" || arg == "internal" || arg == "pc") {
    const DeviceInfo *pick = nullptr;
    FormFactor want = mic ? FormFactor::Microphone : FormFactor::Speakers;
    for (auto &d : list) if (d.bus == DeviceBus::BuiltIn && d.form == want) { pick = &d; break; }
    if (!pick) for (auto &d : list) if (d.bus == DeviceBus::BuiltIn) { pick = &d; break; }
    if (!pick) {
      if (message) *message = std::string("No built-in ") + (mic ? "microphone" : "speakers") + " found";
      return false;
    }
    return set(*pick);
  }
  if (!arg.empty() && std::all_of(arg.begin(), arg.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    size_t n = size_t(atoi(arg.c_str()));
    if (n >= 1 && n <= list.size()) return set(list[n - 1]);
    if (message) *message = "No device #" + arg + " (see: aoi " + (mic ? "mic" : "speaker") + " list)";
    return false;
  }
  std::vector<const DeviceInfo *> hits;
  for (auto &d : list) if (lower_ascii(d.name).find(arg) != std::string::npos) hits.push_back(&d);
  if (hits.size() == 1) return set(*hits[0]);
  if (message) {
    if (hits.empty()) *message = "No " + std::string(mic ? "microphone" : "output") + " matches '" + arg_in + "'";
    else {
      *message = "'" + arg_in + "' matches " + std::to_string(hits.size()) + " devices:";
      for (auto *d : hits) *message += " \"" + d->name + "\"";
      *message += "; use the number from: aoi " + std::string(mic ? "mic" : "speaker") + " list";
    }
  }
  return false;
}

std::string bluetooth_note(const DeviceInfo &mic) {
  if (mic.bus != DeviceBus::Bluetooth || !mic.bluetooth_hfp) return {};
  return "Using " + short_name(mic.name) +
         " mic: their playback switches to call quality while the mic is on; listeners are not affected. "
         "`aoi mic laptop` to use the laptop mic instead.";
}

// ---- persistence -------------------------------------------------------------------
DevicePrefs DevicePrefs::load() {
  DevicePrefs p;
  std::string s;
  if (!read_file(data_dir() / "devices.json", s)) return p;
  json j = json::parse(s, nullptr, false);
  if (!j.is_object()) return p;
  auto dev = [&](const char *k, std::string &id, std::string &name) {
    if (j.contains(k) && j[k].is_object()) { id = j[k].value("id", ""); name = j[k].value("name", ""); }
  };
  dev("mic", p.mic_id, p.mic_name);
  dev("speaker", p.speaker_id, p.speaker_name);
  p.mic_on = j.value("mic_on", true);
  if (j.contains("agc_gain_db") && j["agc_gain_db"].is_object())
    for (auto &[k, v] : j["agc_gain_db"].items())
      if (v.is_number()) p.agc_gain_db[k] = std::clamp(v.get<float>(), 0.f, 30.f);
  return p;
}

bool DevicePrefs::save() const {
  auto dev = [](const std::string &id, const std::string &name) -> json {
    if (id.empty() && name.empty()) return "auto";
    return {{"id", id}, {"name", name}};
  };
  json g = json::object();
  for (auto &[k, v] : agc_gain_db) g[k] = std::round(v * 10) / 10;
  json j = {{"mic", dev(mic_id, mic_name)}, {"speaker", dev(speaker_id, speaker_name)}, {"mic_on", mic_on},
            {"agc_gain_db", g}};
  return write_file_atomic(data_dir() / "devices.json", j.dump(2));
}

}  // namespace aoi
