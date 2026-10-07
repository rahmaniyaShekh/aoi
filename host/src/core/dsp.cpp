#include "dsp.h"

#include <speex_resampler.h>

#include <algorithm>
#include <cstring>

namespace aoi {

// ---- Biquad (RBJ cookbook) ---------------------------------------------------------
Biquad Biquad::highpass(double fc, double q, double fs) {
  double w0 = 2 * M_PI * fc / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
  Biquad b;
  b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0;
  b.a1 = -2 * c / a0; b.a2 = (1 - alpha) / a0;
  return b;
}
Biquad Biquad::lowpass(double fc, double q, double fs) {
  double w0 = 2 * M_PI * fc / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
  Biquad b;
  b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0; b.b2 = (1 - c) / 2 / a0;
  b.a1 = -2 * c / a0; b.a2 = (1 - alpha) / a0;
  return b;
}

// ---- Loudness (BS.1770 K-weighting at 48 kHz) ---------------------------------------
void Loudness::process(const float *x, int frames) {
  if (!init_) {
    for (int c = 0; c < 2; ++c) {
      Biquad &s = shelf_[c];
      s.b0 = 1.53512485958697; s.b1 = -2.69169618940638; s.b2 = 1.19839281085285;
      s.a1 = -1.69065929318241; s.a2 = 0.73248077421585;
      Biquad &h = hp_[c];
      h.b0 = 1.0; h.b1 = -2.0; h.b2 = 1.0;
      h.a1 = -1.99004745483398; h.a2 = 0.99007225036621;
    }
    init_ = true;
  }
  for (int i = 0; i < frames; ++i) {
    double l = hp_[0].run(shelf_[0].run(x[2 * i]));
    double r = hp_[1].run(shelf_[1].run(x[2 * i + 1]));
    acc_ += l * l + r * r;
    if (++acc_n_ == kRate / 10) {  // 100 ms step
      blocks_.push_back(acc_ / acc_n_);
      acc_ = 0; acc_n_ = 0;
      if (blocks_.size() > 4) blocks_.pop_front();
      if (blocks_.size() == 4) {
        double ms = (blocks_[0] + blocks_[1] + blocks_[2] + blocks_[3]) / 4;
        float m = float(-0.691 + 10 * std::log10(ms + 1e-12));
        // Absolute gate: near-silence says nothing about loudness.
        if (m > -60.f) momentary_.push_back(m);
        if (momentary_.size() > 40) momentary_.pop_front();
        if (momentary_.size() >= 8) {
          std::vector<float> v(momentary_.begin(), momentary_.end());
          // Relative gate as in BS.1770: drop what is 20 dB under the loudest.
          float top = *std::max_element(v.begin(), v.end());
          v.erase(std::remove_if(v.begin(), v.end(), [&](float a) { return a < top - 20.f; }), v.end());
          size_t k = size_t(0.7 * double(v.size() - 1));
          std::nth_element(v.begin(), v.begin() + k, v.end());
          lufs_ = v[k];
          valid_ = true;
        } else {
          valid_ = false;
        }
      }
    }
  }
}

// ---- AutoLevel --------------------------------------------------------------------
AutoLevel::AutoLevel(float target, float min_db, float max_db) : target_(target), min_db_(min_db), max_db_(max_db) {}

void AutoLevel::process(float *x, int frames) {
  meter_.process(x, frames);  // open loop: measure the source, not our own output
  float want = gain_db_;
  if (!enabled_) want = 0.f;
  else if (meter_.valid()) want = std::clamp(target_ - meter_.lufs(), min_db_, max_db_);
  float dt = float(frames) / kRate;
  // 1.5 dB/s near target (far too slow to follow the music's own dynamics),
  // 6 dB/s while still far off so a quiet source is rescued in seconds.
  float rate = std::fabs(want - gain_db_) > 6.f ? 6.f : 1.5f;
  float step = rate * dt;
  float g0 = db_to_gain(gain_db_);
  if (want > gain_db_) gain_db_ = std::min(want, gain_db_ + step);
  else gain_db_ = std::max(want, gain_db_ - step);
  float g1 = db_to_gain(gain_db_);
  if (g0 == 1.f && g1 == 1.f) return;
  for (int i = 0; i < frames; ++i) {
    float g = g0 + (g1 - g0) * float(i) / float(frames);
    x[2 * i] *= g; x[2 * i + 1] *= g;
  }
}

// ---- Limiter --------------------------------------------------------------------------
Limiter::Limiter(float ceiling_db, float lookahead_ms, float release_ms) {
  ceiling_ = db_to_gain(ceiling_db);
  L_ = std::max(2, int(lookahead_ms * kRate / 1000));
  rel_ = std::exp(-1.0 / (release_ms * kRate / 1000.0));
  delay_.assign(size_t(L_) * 2, 0.f);
  req_.assign(size_t(L_) + 1, 1.f);
  idx_.assign(size_t(L_) + 1, 0);
  box_.assign(size_t(L_), 1.0);
  bsum_ = L_;
}

void Limiter::process(float *x, int frames) {
  const int cap = L_ + 1;
  for (int i = 0; i < frames; ++i, ++n_) {
    float l = x[2 * i], r = x[2 * i + 1];
    float peak = std::max(std::fabs(l), std::fabs(r));
    float need = peak > ceiling_ ? ceiling_ / peak : 1.f;
    // sliding minimum over the last L_ samples (monotonic deque)
    while (qt_ > qh_ && req_[(qt_ - 1) % cap] >= need) --qt_;
    req_[qt_ % cap] = need; idx_[qt_ % cap] = n_; ++qt_;
    while (idx_[qh_ % cap] <= n_ - L_) ++qh_;
    double m = req_[qh_ % cap];
    // instant attack at the min stage (the box filter smooths it), exponential release
    env_ = m < env_ ? m : m + (env_ - m) * rel_;
    bsum_ += env_ - box_[bpos_];
    box_[bpos_] = env_;
    bpos_ = (bpos_ + 1) % L_;
    if ((n_ & 0xFFFF) == 0) { bsum_ = 0; for (double v : box_) bsum_ += v; }
    float g = float(bsum_ / L_);
    if (g < worst_) worst_ = g;
    // delay line: output the sample written L_-1 steps ago
    int w = int(n_ % L_);
    delay_[2 * w] = l; delay_[2 * w + 1] = r;
    int rd = int((n_ + 1) % L_);
    float ol = delay_[2 * rd] * g, orr = delay_[2 * rd + 1] * g;
    x[2 * i] = std::clamp(ol, -ceiling_, ceiling_);
    x[2 * i + 1] = std::clamp(orr, -ceiling_, ceiling_);
  }
}

float Limiter::take_reduction_db() {
  float w = worst_;
  worst_ = 1.f;
  return -gain_to_db(w);
}

// ---- meter ------------------------------------------------------------------------------
void LevelMeter::process(const float *x, int frames, int channels) {
  float pk[2] = {0, 0};
  double ss[2] = {0, 0};
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < std::min(channels, 2); ++c) {
      float v = x[i * channels + c];
      pk[c] = std::max(pk[c], std::fabs(v));
      ss[c] += double(v) * v;
    }
  std::lock_guard lk(mu);
  for (int c = 0; c < 2; ++c) {
    int src = channels == 1 ? 0 : c;
    peak[c] = std::max(peak[c], pk[src]);
    rms[c] = std::max(rms[c], float(std::sqrt(ss[src] / std::max(frames, 1))));
  }
}

void LevelMeter::snapshot(float p[2], float r[2]) {
  std::lock_guard lk(mu);
  for (int c = 0; c < 2; ++c) { p[c] = peak[c]; r[c] = rms[c]; peak[c] = 0; rms[c] = 0; }
}

// ---- voice chain ----------------------------------------------------------------------------
VoiceChain::VoiceChain() {
  // 4th-order Butterworth high-pass at 80 Hz: desk thumps and HVAC rumble out.
  hp1_ = Biquad::highpass(80, 0.5412);
  hp2_ = Biquad::highpass(80, 1.3066);
}

void VoiceChain::process(float *x, int n) {
  double ss = 0;
  for (int i = 0; i < n; ++i) {
    float v = float(hp2_.run(hp1_.run(x[i])));
    x[i] = v;
    ss += double(v) * v;
  }
  float rms = float(std::sqrt(ss / std::max(n, 1)));
  level_db_ = gain_to_db(rms);
  hist_.push_back(rms);
  if (hist_.size() > 300) hist_.pop_front();  // 3 s of 10 ms blocks
  if (hist_.size() >= 20 && hist_.size() % 10 == 0) {
    std::vector<float> v(hist_.begin(), hist_.end());
    size_t k = v.size() / 10;  // 10th percentile = the room between words
    std::nth_element(v.begin(), v.begin() + k, v.end());
    floor_ = std::max(v[k], 1e-5f);
  }
  bool speech = rms > std::max(floor_ * 4.f, db_to_gain(-52.f));  // 12 dB over the room
  if (speech) hold_ = 30; else if (hold_ > 0) --hold_;
  active_ = hold_ > 0;

  // speech-only makeup gain toward ~-22 dBFS RMS, 3 dB/s, ignores the room
  if (speech) {
    float want = std::clamp(-22.f - level_db_, -6.f, 18.f);
    float step = 3.f * float(n) / kRate;
    agc_db_ = want > agc_db_ ? std::min(want, agc_db_ + step) : std::max(want, agc_db_ - step);
  }
  float gate_target = (!gate_on_ || active_) ? 1.f : db_to_gain(-18.f);
  float g0 = gate_gain_;
  float coef = gate_target > g0 ? 0.5f : 0.06f;  // open in ~20 ms, close in ~170 ms
  gate_gain_ = g0 + (gate_target - g0) * coef;
  float a0 = db_to_gain(agc_db_);
  for (int i = 0; i < n; ++i) {
    float g = (g0 + (gate_gain_ - g0) * float(i) / float(n)) * a0;
    float v = x[i] * g;
    // linear below 0.5, tanh knee above: a shout never clips the bus
    float a = std::fabs(v);
    if (a > 0.5f) v = std::copysign(0.5f + 0.25f * std::tanh((a - 0.5f) / 0.25f), v);
    x[i] = v;
  }
}

void Ducker::process(float *x, int frames, bool voice, float depth_db) {
  if (voice) hold_ = 30; else if (hold_ > 0) --hold_;
  float target = (hold_ > 0 && depth_db > 0) ? db_to_gain(-depth_db) : 1.f;
  float g0 = g_;
  float coef = target < g0 ? 0.4f : 0.025f;  // ~20 ms attack, ~400 ms release per 10 ms block
  g_ = g0 + (target - g0) * coef;
  if (g0 == 1.f && g_ == 1.f) return;
  for (int i = 0; i < frames; ++i) {
    float g = g0 + (g_ - g0) * float(i) / float(frames);
    x[2 * i] *= g; x[2 * i + 1] *= g;
  }
}

// ---- resampler ----------------------------------------------------------------------------
Resampler::Resampler(int channels, int in_rate, int out_rate, int quality)
    : ch_(channels), in_rate_(in_rate), out_rate_(out_rate) {
  if (in_rate != out_rate) {
    int err = 0;
    st_ = speex_resampler_init(static_cast<spx_uint32_t>(channels), static_cast<spx_uint32_t>(in_rate),
                               static_cast<spx_uint32_t>(out_rate), quality, &err);
    if (st_) speex_resampler_skip_zeros(st_);
  }
}
Resampler::~Resampler() { if (st_) speex_resampler_destroy(st_); }

void Resampler::process(const float *in, int frames, std::vector<float> &out) {
  if (!st_) { out.insert(out.end(), in, in + size_t(frames) * ch_); return; }
  while (frames > 0) {
    spx_uint32_t in_len = static_cast<spx_uint32_t>(frames);
    spx_uint32_t out_len = static_cast<spx_uint32_t>(int64_t(frames) * out_rate_ / in_rate_ + 16);
    size_t at = out.size();
    out.resize(at + size_t(out_len) * ch_);
    speex_resampler_process_interleaved_float(st_, in, &in_len, out.data() + at, &out_len);
    out.resize(at + size_t(out_len) * ch_);
    in += size_t(in_len) * ch_;
    frames -= int(in_len);
    if (in_len == 0) break;
  }
}

// ---- FracRing / DriftReader -------------------------------------------------------------------
FracRing::FracRing(int capacity) : buf_(size_t(capacity), 0.f) {}

void FracRing::write(const float *x, int n) {
  const int64_t cap = int64_t(buf_.size());
  for (int i = 0; i < n; ++i) buf_[size_t(wpos_++ % cap)] = x[i];
  // Overrun: the oldest audio is gone; move the reader past it.
  if (double(wpos_) - rpos_ > double(cap - 4)) rpos_ = double(wpos_ - cap / 2);
}

int FracRing::read(float *out, int n, double ratio) {
  const int64_t cap = int64_t(buf_.size());
  int i = 0;
  for (; i < n; ++i) {
    int64_t k = int64_t(rpos_);
    if (k + 2 >= wpos_ || k < 1) break;
    double t = rpos_ - double(k);
    float p0 = buf_[size_t((k - 1) % cap)], p1 = buf_[size_t(k % cap)];
    float p2 = buf_[size_t((k + 1) % cap)], p3 = buf_[size_t((k + 2) % cap)];
    out[i] = float(p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 +
                                                   t * (3.0 * (p1 - p2) + p3 - p0))));
    rpos_ += ratio;
  }
  return i;
}

DriftReader::DriftReader(int capacity) : ring_(capacity) {}

void DriftReader::write(const float *x, int n) {
  std::lock_guard lk(mu_);
  ring_.write(x, n);
}

int DriftReader::fill() const {
  std::lock_guard lk(mu_);
  return int(ring_.avail());
}

void DriftReader::reset() {
  std::lock_guard lk(mu_);
  ring_.reset();
  primed_ = false;
  integ_ = 0;
  sfill_ = -1;
  ratio_ = 1.0;
}

bool DriftReader::read(float *out, int n) {
  std::lock_guard lk(mu_);
  double fill = ring_.avail();
  if (!primed_) {
    if (fill < target_ + n) { std::fill(out, out + n, 0.f); return false; }
    primed_ = true;
    sfill_ = -1;
  }
  // Far too full (a stall that cleared all at once): skip to the target.
  if (fill > target_ * 3 + kRate / 10) { ring_.skip(fill - target_); fill = target_; }
  // Servo on a smoothed fill: capture devices deliver in lumps, and chasing
  // each lump would wobble the pitch for nothing.
  sfill_ = sfill_ < 0 ? fill : sfill_ + 0.08 * (fill - sfill_);
  double err = (sfill_ - target_) / std::max(target_, 1);
  integ_ = std::clamp(integ_ + err * n / kRate, -3.0, 3.0);
  ratio_ = 1.0 + std::clamp(0.008 * err + 0.004 * integ_, -0.006, 0.006);
  int got = ring_.read(out, n, ratio_);
  if (got < n) {
    // Fade the tail rather than stepping to zero, then wait to re-prime.
    float last = got > 0 ? out[got - 1] : 0.f;
    for (int i = got; i < n; ++i) { last *= 0.95f; out[i] = last; }
    primed_ = false;
    ++underruns_;
    return false;
  }
  return true;
}

}  // namespace aoi
