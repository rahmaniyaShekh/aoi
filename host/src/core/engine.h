// The host: capture -> condition -> fan out to listeners; signalling; talkback;
// the microphone and talkback devices, chosen and followed live.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "devices.h"
#include "dsp.h"
#include "listener.h"
#include "netsim.h"
#include "platform/audio_system.h"
#include "rendezvous.h"
#include "voice.h"

namespace aoi {

struct Settings {
  std::string service = "https://share.mdarif.online/aoi";
  // Everything on by default: no configuration is needed to get music, voice
  // and talkback. A PC with no microphone simply sends music.
  bool mic = true;            // the microphone may be mixed in (the owner turns it off with `aoi mic off`)
  bool talkback = true;       // listeners may talk back
  bool auto_level = true;
  int max_kbps = 256;         // host-side quality cap per listener (adapts below it)
  int max_listeners = 2;
  float talk_gain_db = 0.f;
  NetSimConfig sim;
  bool new_code = false;
  // Testing (`aoi echotest`): no code is published; `test_talk` plays as if a
  // listener were talking back; `mic_tap` sees each 10 ms mic block before
  // and after voice processing (mic thread).
  bool offline = false;
  bool split = false;  // `--split`: voice left, PC sound right; the listener mixes
  std::function<void(float *mono, int frames)> test_talk;
  std::function<void(const float *raw, const float *processed, int frames)> mic_tap;
  std::function<void(const float *ref, int frames)> ref_tap;  // the echo reference, as fed
  float test_level_db = 0.f;
};

// One device as `aoi status` / `aoi mic` show it.
struct DeviceView {
  std::string id, name, type, reason, error, note;
  bool present = false, running = false, access_denied = false;
  int rate = 0, channels = 0;
  std::string format;
  double latency_ms = 0;
};

struct Snapshot {
  std::string code, pretty, link, status, source, mic_name, talk_device, error;
  bool published = false, socket = false, turn = false, process_loopback = false;
  bool mic = false, talkback = false, auto_level = false, mic_active = false;
  int max_kbps = 256, max_listeners = 2, failed_connects = 0;
  double last_failed_s = -1;
  float peak[2]{}, rms[2]{}, mic_level_db = -90, gain_db = 0, source_lufs = -70, limiter_db = 0;
  DeviceView mic_dev, speaker_dev;
  std::vector<std::string> echo_reference;  // devices whose output is cancelled from the mic
  std::optional<float> mic_volume;          // the endpoint's own input level, 0..1
  bool mic_switching = false;
  bool split = false;
  VoiceStats voice;
  std::vector<ListenerView> listeners;
};

class Engine {
 public:
  explicit Engine(Settings s);
  ~Engine();
  void start();
  void stop();

  void set_mic(bool on);
  void set_talkback(bool on);
  void set_auto_level(bool on);
  void set_max_kbps(int k);
  void rotate_code();
  void kick(int id);
  // `aoi mic <arg>` / `aoi speaker <arg>` on the running host: applied live
  // and persisted. Returns false (with a message) when nothing matched.
  bool choose_device(DeviceKind kind, const std::string &arg, std::string *message);
  std::vector<DeviceInfo> devices(DeviceKind kind);
  Snapshot snapshot();
  const Settings &settings() const { return settings_; }

 private:
  struct MicChannel;

  void on_system(const float *stereo, int frames);
  void read_mic(float *out, bool *voice);
  void render_pull(float *mono, int frames);
  void on_reference(const float *mono, int frames);
  void on_secondary_reference(const float *mono, int frames);
  void feed_reference(const float *block);
  void host_loop();
  void housekeeping();
  void device_loop();
  void reevaluate();
  void open_mic(const DeviceInfo &d);
  void close_mic();
  void open_output(const std::optional<DeviceInfo> &spk, const std::optional<DeviceInfo> &secondary);
  void save_agc(const MicChannel &ch);
  void wake_devices();
  bool load_identity(bool fresh);
  std::vector<std::shared_ptr<Listener>> listeners_copy();
  void set_status(const std::string &s, const std::string &err = {});

  Settings settings_;
  std::unique_ptr<IAudioSystem> audio_;
  std::unique_ptr<SystemCaptureStream> capture_;
  AutoLevel level_;
  LevelMeter meter_;
  std::atomic<float> mic_level_db_{-90.f};
  std::atomic<bool> mic_active_{false};

  // ---- devices (guarded by dev_mu_ unless noted) ----
  std::mutex dev_mu_;
  std::condition_variable dev_cv_;
  bool dev_dirty_ = true;
  int64_t dev_dirty_at_ = 0;
  DevicePrefs prefs_;
  DeviceChoice mic_choice_, spk_choice_;
  std::optional<float> mic_volume_;
  int event_token_ = 0;
  std::thread device_thread_;

  // Microphone channels: `cur_` plays; `next_` is a newly opened device that
  // takes over with a 20 ms crossfade once it has primed.
  std::mutex mic_mu_;
  std::shared_ptr<MicChannel> cur_, next_;
  std::vector<std::shared_ptr<MicChannel>> retired_;
  float mic_gain_ = 0.f;  // capture thread only: fades over gaps

  // Talkback output + the echo reference: what that device (and the default
  // speakers, if they differ) actually plays, taken from its loopback after
  // every app's audio, ours and the device's effects are mixed. That is the
  // signal the room hears, with one consistent delay to the microphone.
  std::mutex out_mu_;
  std::unique_ptr<AudioStream> render_;
  std::string render_id_;
  std::unique_ptr<AudioStream> ref_loop_, ref_loop2_;
  std::string ref_id_, ref2_id_;
  std::string ref_name_, ref2_name_;
  DriftReader ref2_reader_{kRate};
  std::atomic<bool> ref2_on_{false};
  std::atomic<bool> room_echo_{true};  // sound plays on speakers the mic can hear
  std::vector<float> ref_acc_;  // reference thread only

  // shared with listeners (they hold pointers)
  std::atomic<bool> talk_allowed_{true}, mic_on_{false};
  std::atomic<int> max_kbps_{256}, listener_count_{0}, connecting_count_{0}, failed_connects_{0};
  std::atomic<int64_t> last_failed_us_{0};
  std::atomic<bool> refresh_ice_{false};

  std::mutex mu_;
  std::vector<std::shared_ptr<Listener>> listeners_;
  std::vector<std::pair<int64_t, std::shared_ptr<Listener>>> graveyard_;
  int next_id_ = 1;

  std::string code_, owner_;
  std::shared_ptr<Rendezvous> rdv_;  // shared: relayed listeners hold it weakly
  std::atomic<bool> stopping_{false};
  std::atomic<bool> running_{false}, rotate_{false}, cancel_wait_{false}, published_{false}, turn_{false};
  std::thread host_thread_, house_thread_;
  std::mutex status_mu_;
  std::string status_ = "Starting", error_;
  std::vector<float> talk_mix_;
};

}  // namespace aoi
