// The host: capture -> condition -> fan out to listeners; signalling; talkback.
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "dsp.h"
#include "listener.h"
#include "netsim.h"
#include "rendezvous.h"

namespace aoi {

struct Settings {
  std::string service = "https://share.mdarif.online/aoi";
  // Everything on by default: no configuration is needed to get music, voice
  // and talkback. A PC with no microphone simply sends music.
  bool mic = true;            // mix the host's microphone in
  bool talkback = true;       // listeners may talk back
  bool auto_level = true;
  int max_kbps = 256;         // host-side quality cap per listener (adapts below it)
  int max_listeners = 2;
  float talk_gain_db = 0.f;
  NetSimConfig sim;
  bool new_code = false;
};

struct Snapshot {
  std::string code, pretty, link, status, source, mic_name, talk_device, error;
  bool published = false, socket = false, turn = false, process_loopback = false;
  bool mic = false, talkback = false, auto_level = false, mic_active = false;
  int max_kbps = 256, max_listeners = 2;
  float peak[2]{}, rms[2]{}, mic_level_db = -90, gain_db = 0, source_lufs = -70, limiter_db = 0;
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
  Snapshot snapshot();
  const Settings &settings() const { return settings_; }

 private:
  void on_system(const float *stereo, int frames);
  void render_pull(float *mono, int frames);
  void host_loop();
  void housekeeping();
  bool load_identity(bool fresh);
  std::vector<std::shared_ptr<Listener>> listeners_copy();
  void set_status(const std::string &s, const std::string &err = {});

  Settings settings_;
  std::unique_ptr<SystemCapture> capture_;
  std::unique_ptr<MicCapture> mic_;
  std::unique_ptr<Render> render_;
  DriftReader mic_reader_{kRate * 2};
  VoiceChain voice_;
  AutoLevel level_;
  LevelMeter meter_;
  std::atomic<float> mic_level_db_{-90.f};
  std::atomic<bool> mic_active_{false};

  // shared with listeners (they hold pointers)
  std::atomic<bool> talk_allowed_{true}, mic_on_{false};
  std::atomic<int> max_kbps_{256}, listener_count_{0};

  std::mutex mu_;
  std::vector<std::shared_ptr<Listener>> listeners_;
  std::vector<std::pair<int64_t, std::shared_ptr<Listener>>> graveyard_;
  int next_id_ = 1;

  std::string code_, owner_;
  std::unique_ptr<Rendezvous> rdv_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> running_{false}, rotate_{false}, cancel_wait_{false}, published_{false}, turn_{false};
  std::thread host_thread_, house_thread_;
  std::mutex status_mu_;
  std::string status_ = "Starting", error_;
  std::vector<float> talk_mix_;
};

}  // namespace aoi
