// WASAPI capture and render.
//
// SystemCapture taps "what this PC is playing" with *process loopback* in
// EXCLUDE mode against our own process tree: every application's audio, taken
// before the endpoint's effects chain (no driver compressor), endpoint
// agnostic (Bluetooth connecting mid-song changes nothing), and structurally
// free of anything we play ourselves -- which is what makes talkback on a
// single output device possible without the listener hearing themselves.
// If process loopback is refused it falls back to endpoint loopback that
// follows the default output device.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aoi {

class SystemCapture {
 public:
  // Called on the capture thread with exactly kBlock (480) frames of 48 kHz
  // interleaved stereo float.
  using Callback = std::function<void(const float *stereo, int frames)>;
  explicit SystemCapture(Callback cb) : cb_(std::move(cb)) {}
  ~SystemCapture() { stop(); }
  void start();
  void stop();
  std::string source() const { std::lock_guard lk(mu_); return source_; }
  bool process_loopback() const { return process_loopback_; }
  uint64_t blocks() const { return blocks_; }
  int restarts() const { return restarts_; }

 private:
  void run();
  bool run_process_loopback();
  void run_endpoint_loopback();
  void emit(const float *stereo, int frames);
  Callback cb_;
  std::thread thread_;
  std::atomic<bool> stop_{false}, process_loopback_{false};
  std::atomic<uint64_t> blocks_{0};
  std::atomic<int> restarts_{0};
  mutable std::mutex mu_;
  std::string source_ = "starting";
  std::vector<float> acc_;
};

class MicCapture {
 public:
  // Mono 48 kHz, arbitrary block sizes.
  using Callback = std::function<void(const float *mono, int frames)>;
  explicit MicCapture(Callback cb) : cb_(std::move(cb)) {}
  ~MicCapture() { stop(); }
  void start();
  void stop();
  bool running() const { return running_; }
  std::string name() const { std::lock_guard lk(mu_); return name_; }

 private:
  void run();
  Callback cb_;
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false};
  mutable std::mutex mu_;
  std::string name_;
};

class Render {
 public:
  // Pull mono 48 kHz samples; called on the render thread.
  using Pull = std::function<void(float *mono, int frames)>;
  explicit Render(Pull p) : pull_(std::move(p)) {}
  ~Render() { stop(); }
  void start();
  void stop();
  bool running() const { return running_; }
  std::string name() const { std::lock_guard lk(mu_); return name_; }

 private:
  void run();
  Pull pull_;
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false};
  mutable std::mutex mu_;
  std::string name_;
};

// Bumped whenever Windows changes the default render/capture device.
extern std::atomic<int> g_default_render_changes, g_default_capture_changes;
void audio_watch_devices();

}  // namespace aoi
