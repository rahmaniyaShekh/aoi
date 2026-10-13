// Which microphone and which talkback output to use, and why.
//
// Auto (the default, nothing to configure) picks the mic in this order:
//   1. the owner's explicit choice, if set and connected;
//   2. a connected USB or wired headset mic;
//   3. a Bluetooth LE Audio headset mic (stereo playback stays on);
//   4. Windows' default capture device, then the communications one;
//   5. a classic Bluetooth headset mic, only if it is the only mic (opening
//      it drops the headset's playback to call quality).
// Talkback output: the explicit choice; else the output of the same physical
// device as the mic (voices come out of the same headset); else Windows'
// default output, where the owner is listening; else the communications one.
//
// The policy is a pure function of the device lists, so the selftest drives
// it with fake lists.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "platform/audio_system.h"

namespace aoi {

// Persisted in data_dir()/devices.json. Empty ids mean "auto".
struct DevicePrefs {
  std::string mic_id, mic_name;          // explicit microphone
  std::string speaker_id, speaker_name;  // explicit talkback output
  bool mic_on = true;                    // mix the microphone in at all
  // Per microphone: the voice level the auto gain settled on last time, so
  // the next start begins there instead of re-learning the mic.
  std::map<std::string, float> agc_gain_db;

  static DevicePrefs load();
  bool save() const;
};

struct DeviceChoice {
  std::optional<DeviceInfo> device;  // none: no device of that kind
  std::string reason;                // "auto: Bluetooth headset", "your choice"
  bool chosen_missing = false;       // an explicit choice that is not connected now
};

DeviceChoice choose_mic(const std::vector<DeviceInfo> &captures, const DevicePrefs &prefs);
DeviceChoice choose_speaker(const std::vector<DeviceInfo> &renders, const std::optional<DeviceInfo> &mic,
                            const DevicePrefs &prefs);

// "Bluetooth headset", "USB microphone", "built-in microphone", "speakers"...
std::string device_type(const DeviceInfo &d);
const char *to_string(DeviceBus b);
const char *to_string(FormFactor f);

// What `aoi mic <arg>` / `aoi speaker <arg>` means, applied to prefs.
// `arg` is a list number (as printed by `aoi mic list`), part of a name,
// "auto", "laptop"/"builtin" (the PC's own device), or for the mic "on"/"off".
// Returns false with a message when nothing matched.
bool apply_device_choice(DevicePrefs &prefs, DeviceKind kind, const std::vector<DeviceInfo> &list,
                         const std::string &arg, std::string *message);

// The one line the owner should read when the mic is a classic Bluetooth
// headset (the hands-free profile drops its playback to call quality).
std::string bluetooth_note(const DeviceInfo &mic);

}  // namespace aoi
