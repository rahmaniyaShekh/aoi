// Voice processing for the host's microphone: WebRTC Audio Processing
// (freedesktop webrtc-audio-processing 2.1, BSD-3), per 10 ms block of
// 48 kHz mono:
//
//   mic -> [APM: high-pass -> AEC3 echo canceller -> noise suppression ->
//          AGC2 adaptive digital gain] -> -2 dB headroom -> soft residual-echo
//          expander (only while the echo reference plays, at most -8 dB,
//          keyed by the RNN voice detector; never a gate) -> soft safety knee
//
// The echo reference is what the talkback output device actually plays
// (endpoint loopback: the music the owner hears *and* the listeners' voices),
// fed to reverse() from its own thread. AEC3 aligns the two clocks itself
// (delay estimator + render buffer), as it does in Chrome where capture and
// playback are different devices too; set_delay_ms() gives it the measured
// device latencies as a starting point.
//
// Voice activity for ducking comes from the same RNN voice detector AGC2
// uses, run on the processed signal, so music, keyboard and fan noise cannot
// trigger it.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace aoi {

struct VoiceOptions {
  bool aec = true, ns = true, agc = true, expander = true;
  float initial_gain_db = 22.f;  // where AGC2 starts (the last converged value for a known mic)
};

struct VoiceStats {
  bool apm = false;  // WebRTC APM (false: the simple fallback chain)
  bool aec = false, ns = false, agc = false;
  bool reference_active = false;    // the echo reference carries sound right now
  bool echo = false;                // AEC3 sees echo (ERL measurable) in the last seconds
  std::optional<double> erle_db;    // AEC3's echo return loss enhancement
  std::optional<double> erl_db;     // echo path loss, speaker -> mic
  std::optional<int> delay_ms;      // AEC3's render buffer delay
  std::optional<double> residual_echo;
  float speech_prob = 0;
  bool voice = false;
  float in_speech_dbfs = -90, out_speech_dbfs = -90;  // smoothed over speech
  float gain_db = 0;                // what AGC2 applies now (estimated)
  // Measured on the real device: while the reference plays and nobody here
  // talks, how much quieter the processed mic is than the raw mic (ERLE as
  // the listener experiences it), over the last ~10 s; and the absolute
  // level of what is left.
  std::optional<float> measured_erle_db;
  float echo_in_dbfs = -90, echo_out_dbfs = -90;
  double speech_seconds = 0;
  float cpu_avg_us = 0, cpu_max_us = 0;  // per 10 ms block, capture + reference
  uint64_t blocks = 0, reverse_blocks = 0;
};

class VoiceProcessor {
 public:
  static constexpr int kFrames = 480;  // 10 ms at 48 kHz
  static constexpr int kCaptureLeadMs = 40;  // the mic is held back this much so the echo reference leads
  explicit VoiceProcessor(VoiceOptions o = {});
  ~VoiceProcessor();
  VoiceProcessor(const VoiceProcessor &) = delete;
  VoiceProcessor &operator=(const VoiceProcessor &) = delete;

  void reverse(const float *block);  // echo reference, any thread
  void process(float *block);        // microphone, in place, one thread
  void set_delay_ms(int ms) { delay_ms_ = ms; }
  bool voice_active() const { return voice_; }
  VoiceStats stats() const;
  static bool apm_available();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<int> delay_ms_{60};
  std::atomic<bool> voice_{false};
};

}  // namespace aoi
