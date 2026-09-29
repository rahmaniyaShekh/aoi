// Signal processing for the send path (and the talkback return path).
//
// Everything runs on 48 kHz float. The send path is interleaved stereo; mic
// and talkback paths are mono.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct SpeexResamplerState_;

namespace aoi {

constexpr int kRate = 48000;
constexpr int kBlock = 480;  // 10 ms, the pipeline's unit of work

inline float db_to_gain(float db) { return std::pow(10.f, db / 20.f); }
inline float gain_to_db(float g) { return 20.f * std::log10(std::max(g, 1e-9f)); }

// Direct form II transposed biquad.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  inline double run(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  static Biquad highpass(double fc, double q = 0.7071, double fs = kRate);
  static Biquad lowpass(double fc, double q = 0.7071, double fs = kRate);
};

// ITU-R BS.1770 K-weighted loudness of a stereo stream, as a short-term
// estimate in LUFS: what a listener perceives as "how loud", which plain RMS
// gets wrong for bass-heavy or bright material.
class Loudness {
 public:
  void process(const float *stereo, int frames);
  // 70th percentile of gated momentary loudness over the last ~4 s.
  float lufs() const { return lufs_; }
  bool valid() const { return valid_; }

 private:
  Biquad shelf_[2], hp_[2];
  bool init_ = false;
  double acc_ = 0; int acc_n_ = 0;      // current 100 ms block
  std::deque<double> blocks_;          // last 4 x 100 ms mean squares -> 400 ms momentary
  std::deque<float> momentary_;        // last ~4 s of momentary loudness
  float lufs_ = -70; bool valid_ = false;
};

// Slow makeup gain toward a loudness target. Not a compressor: within a block
// the gain is one number, so the music's own dynamics survive; it moves only
// between blocks at a bounded rate, and silence is not evidence.
class AutoLevel {
 public:
  explicit AutoLevel(float target_lufs = -18.f, float min_db = -10.f, float max_db = 15.f);
  void set_enabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  void set_target(float t) { target_ = t; }
  void process(float *stereo, int frames);  // in place
  float gain_db() const { return gain_db_; }
  float source_lufs() const { return meter_.lufs(); }

 private:
  Loudness meter_;
  bool enabled_ = true;
  float target_, min_db_, max_db_, gain_db_ = 0.f;
};

// Look-ahead brickwall limiter (stereo linked).
//
// Sliding-window minimum of the required gain over the look-ahead, then a box
// filter of the same length: the gain provably reaches what every sample
// needs *before* that sample plays, so there is no overshoot and no zero-attack
// click; at any level that does not clip it is an exact (delayed) wire.
// Ceiling defaults to -1 dBFS: a lossy codec's decoder can overshoot the
// encoder's input by a fraction of a dB, and the listener's DAC clips at 0.
class Limiter {
 public:
  explicit Limiter(float ceiling_db = -1.0f, float lookahead_ms = 5.f, float release_ms = 150.f);
  void process(float *stereo, int frames);  // in place, adds `latency()` frames of delay
  int latency() const { return L_ - 1; }
  float take_reduction_db();                // worst reduction since last call

 private:
  float ceiling_;
  int L_;
  double rel_;
  std::vector<float> delay_;   // interleaved stereo ring, L_ frames
  int dpos_ = 0;
  // monotonic deque for the sliding minimum
  std::vector<float> req_; std::vector<int64_t> idx_;
  int64_t qh_ = 0, qt_ = 0;  // 64-bit: an int wraps after ~12 h at 48 kHz
  std::vector<double> box_; int bpos_ = 0; double bsum_;
  int64_t n_ = 0;
  double env_ = 1.0;
  float worst_ = 1.f;
};

// Per-block peak/RMS for meters.
struct LevelMeter {
  void process(const float *x, int frames, int channels);
  float peak[2] = {0, 0}, rms[2] = {0, 0};
  std::mutex mu;
  void snapshot(float out_peak[2], float out_rms[2]);
};

// Voice path conditioning for the host's microphone: 80 Hz high-pass, a
// noise-floor-tracking gate, speech auto-level and a tanh safety knee.
class VoiceChain {
 public:
  VoiceChain();
  void process(float *mono, int frames);  // in place
  bool active() const { return active_; } // someone is talking right now
  void set_gate(bool on) { gate_on_ = on; }
  float level_db() const { return level_db_; }

 private:
  Biquad hp1_, hp2_;
  float floor_ = 1e-3f;
  int hold_ = 0;
  float gate_gain_ = 1.f;
  bool active_ = false, gate_on_ = true;
  float agc_db_ = 0.f;
  float level_db_ = -90.f;
  std::deque<float> hist_;
};

// Dips the system audio while the voice is present: fast attack so the first
// syllable is not buried, slow release so the music breathes.
class Ducker {
 public:
  void process(float *stereo, int frames, bool voice_active, float depth_db);
  float gain() const { return g_; }
 private:
  float g_ = 1.f;
  int hold_ = 0;
};

// Streaming sample-rate converter (SpeexDSP, windowed sinc).
class Resampler {
 public:
  Resampler(int channels, int in_rate, int out_rate, int quality = 8);
  ~Resampler();
  Resampler(const Resampler &) = delete;
  Resampler &operator=(const Resampler &) = delete;
  // Appends converted interleaved frames to `out`.
  void process(const float *in, int frames, std::vector<float> &out);
  bool passthrough() const { return st_ == nullptr; }

 private:
  SpeexResamplerState_ *st_ = nullptr;
  int ch_, in_rate_, out_rate_;
};

// Mono ring read at a fractional position with 4-point cubic (Catmull-Rom)
// interpolation. Not thread safe; callers lock.
class FracRing {
 public:
  explicit FracRing(int capacity);
  void write(const float *x, int n);
  double avail() const { return double(wpos_) - rpos_; }
  // Reads up to n samples advancing `ratio` input samples per output sample.
  // Returns how many were produced (fewer only if the ring ran dry).
  int read(float *out, int n, double ratio);
  void skip(double samples) { rpos_ += samples; }
  void reset() { wpos_ = 0; rpos_ = 1.0; std::fill(buf_.begin(), buf_.end(), 0.f); }

 private:
  std::vector<float> buf_;
  int64_t wpos_ = 0;
  double rpos_ = 1.0;  // one sample of history for the interpolator
};

// A single-producer/single-consumer ring of mono audio read at a fractional,
// servo-controlled rate. It absorbs the clock drift between two devices (or
// a remote sender and our sound card): a PI controller holds the fill level
// at its target by nudging the read rate within +-0.5 %, far below audibility,
// and the fractional read uses 4-point cubic interpolation. Drift then shows
// up as a slowly creeping interpolation phase instead of a dropped or
// repeated block.
class DriftReader {
 public:
  explicit DriftReader(int capacity_frames = kRate * 2);
  void write(const float *mono, int frames);
  // Fills exactly `frames` samples; returns false (silence written) on underrun.
  bool read(float *out, int frames);
  void set_target(int frames) { target_ = frames; }
  int fill() const;
  int target() const { return target_; }
  double ratio() const { return ratio_; }
  void reset();
  int underruns() const { return underruns_; }

 private:
  mutable std::mutex mu_;
  FracRing ring_;
  int target_ = kRate / 25;  // 40 ms
  double ratio_ = 1.0, integ_ = 0.0, sfill_ = -1;
  bool primed_ = false;
  int underruns_ = 0;
};

}  // namespace aoi
