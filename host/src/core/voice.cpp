#include "voice.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <vector>

#include "dsp.h"
#include "util.h"

#if AOI_HAVE_APM
#include "api/audio/audio_processing.h"
#include "modules/audio_processing/agc2/cpu_features.h"
#include "modules/audio_processing/agc2/vad_wrapper.h"
#include "modules/audio_processing/aec3/echo_canceller3.h"
#endif

namespace aoi {

namespace {
constexpr float kExpanderDepthDb = 8.f;
const float kSpeechFloor = std::pow(10.f, -45.f / 20.f);

float block_rms(const float *x, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i) s += double(x[i]) * x[i];
  return float(std::sqrt(s / std::max(n, 1)));
}
}  // namespace

#if AOI_HAVE_APM
// AEC3, configured for our topology. The reference is the endpoint loopback:
// exactly what the speakers play, effects included. Windows hands it to us
// only after the audio engine has mixed a period, and a raw laptop mic can
// deliver the echo of that period sooner (measured on a Realtek + Intel SST
// laptop: echo 16 ms ahead of its reference, +-10 ms of delivery jitter).
// An echo canceller cannot remove what it has not been shown yet, so the
// microphone is held back 40 ms inside AEC3 (its fixed capture delay, in
// 16 kHz band samples); the reference then always leads, and AEC3's own
// delay estimator finds the rest (up to ~500 ms: Bluetooth speakers too).
// Pre-echo detection is for references that arrive late, which this rules
// out; on music it misfired (selftest: 150 ms bursts of uncancelled echo
// whenever it re-picked a too-short delay; 19-28 dB ERLE with it, 48 dB
// without, and better double talk).
struct Aec3Factory final : webrtc::EchoControlFactory {
  std::unique_ptr<webrtc::EchoControl> Create(int rate, int render_ch, int capture_ch) override {
    webrtc::EchoCanceller3Config c;
    c.delay.fixed_capture_delay_samples = VoiceProcessor::kCaptureLeadMs * 16;
    c.delay.detect_pre_echo = false;
    return std::make_unique<webrtc::EchoCanceller3>(c, std::nullopt, rate, size_t(render_ch), size_t(capture_ch));
  }
};
#endif

struct VoiceProcessor::Impl {
  VoiceOptions opt;
#if AOI_HAVE_APM
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  webrtc::AudioProcessing::Config config;
  std::unique_ptr<webrtc::VoiceActivityDetectorWrapper> vad;
#endif
  std::mutex cfg_mu;  // set_echo_cancel() callers
  VoiceChain fallback;  // only when APM is not compiled in
  std::vector<float> s16;
  // capture-thread state
  float exp_gain = 1.f;
  int speech_hold = 0, voice_hold = 0, speech_run = 0;
  double in_lvl = -90, out_lvl = -90, speech_s = 0;
  double echo_in = 0, echo_out = 0, echo_n = 0;  // echo-only blocks, leaky sums (~10 s)
  float prob = 0;
  // shared
  std::atomic<int64_t> ref_active_until{0};
  std::atomic<int64_t> reverse_us{0};
  std::atomic<uint64_t> reverse_blocks{0};
  // Debug: AOI_VOICE_DUMP=<file> records every block in the order the
  // processor sees it (type, time, 480 floats): 0 reference, 1 raw mic,
  // 2 processed. Two minutes at most.
  std::mutex dump_mu;
  std::ofstream dump;
  int64_t dump_until = 0;
  void record(uint8_t type, const float *x) {
    std::lock_guard lk(dump_mu);
    if (!dump.is_open()) return;
    int64_t t = now_us();
    if (t > dump_until) { dump.close(); return; }
    dump.write(reinterpret_cast<const char *>(&type), 1);
    dump.write(reinterpret_cast<const char *>(&t), 8);
    dump.write(reinterpret_cast<const char *>(x), kFrames * 4);
  }
  mutable std::mutex mu;
  VoiceStats st;
  double cpu_sum = 0;
  uint64_t cpu_n = 0;
  float cpu_max = 0;
};

bool VoiceProcessor::apm_available() {
#if AOI_HAVE_APM
  return true;
#else
  return false;
#endif
}

VoiceProcessor::VoiceProcessor(VoiceOptions o) : impl_(std::make_unique<Impl>()) {
  Impl &m = *impl_;
  m.opt = o;
  m.s16.resize(kFrames);
#if AOI_HAVE_APM
  webrtc::AudioProcessing::Config c;
  c.pipeline.maximum_internal_processing_rate = 48000;
  c.pipeline.multi_channel_capture = false;
  c.pipeline.multi_channel_render = false;
  c.high_pass_filter.enabled = true;
  c.echo_canceller.enabled = o.aec;
  c.echo_canceller.mobile_mode = false;
  c.noise_suppression.enabled = o.ns;
  c.noise_suppression.level = o.gentle_ns ? webrtc::AudioProcessing::Config::NoiseSuppression::kModerate
                                          : webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  c.gain_controller2.enabled = o.agc;
  auto &ad = c.gain_controller2.adaptive_digital;
  ad.enabled = o.agc;
  // Measured (selftest): speech settles at about -18 dBFS after the post
  // gain; a -40 dBFS talker gets there in about 1.5 s.
  ad.headroom_db = 1.f;
  ad.max_gain_db = 36.f;
  ad.initial_gain_db = std::clamp(o.initial_gain_db, 0.f, 30.f);
  ad.max_gain_change_db_per_second = 12.f;
  ad.max_output_noise_level_dbfs = -50.f;
  webrtc::AudioProcessingBuilder builder;
  builder.SetConfig(c);
  if (o.aec) builder.SetEchoControlFactory(std::make_unique<Aec3Factory>());
  m.apm = builder.Create();
  m.config = c;
  m.vad = std::make_unique<webrtc::VoiceActivityDetectorWrapper>(webrtc::GetAvailableCpuFeatures(), 48000);
  m.st.apm = m.apm != nullptr;
#endif
  if (const char *d = getenv("AOI_VOICE_DUMP")) {
    m.dump.open(d, std::ios::binary | std::ios::trunc);
    m.dump_until = now_us() + 120000000;
  }
  m.st.aec = m.st.apm && o.aec;
  m.st.ns = m.st.apm && o.ns;
  m.st.agc = m.st.apm && o.agc;
}

VoiceProcessor::~VoiceProcessor() = default;

void VoiceProcessor::set_echo_cancel(bool on) {
  Impl &m = *impl_;
  std::lock_guard lk(m.cfg_mu);
  if (!m.opt.aec || echo_on_ == on) return;
#if AOI_HAVE_APM
  if (m.apm) {
    m.config.echo_canceller.enabled = on;
    m.apm->ApplyConfig(m.config);
  }
#endif
  echo_on_ = on;
}

void VoiceProcessor::reverse(const float *x) {
  Impl &m = *impl_;
  int64_t t0 = now_us();
  m.record(0, x);
  if (block_rms(x, kFrames) > 0.001f) m.ref_active_until = t0 + 500000;  // above -60 dBFS
#if AOI_HAVE_APM
  if (m.apm && m.opt.aec && echo_on_) {
    webrtc::StreamConfig sc(48000, 1);
    std::vector<float> tmp(x, x + kFrames);
    const float *src[1] = {tmp.data()};
    float *dst[1] = {tmp.data()};
    m.apm->ProcessReverseStream(src, sc, sc, dst);
  }
#endif
  m.reverse_us += now_us() - t0;
  ++m.reverse_blocks;
}

void VoiceProcessor::process(float *x) {
  Impl &m = *impl_;
  int64_t t0 = now_us();
  float in_rms = block_rms(x, kFrames);
  m.record(1, x);
  bool ref_active = t0 < m.ref_active_until;
  float prob = 0;
#if AOI_HAVE_APM
  if (m.apm) {
    webrtc::StreamConfig sc(48000, 1);
    const float *src[1] = {x};
    float *dst[1] = {x};
    if (m.opt.aec && echo_on_) m.apm->set_stream_delay_ms(delay_ms_);
    m.apm->ProcessStream(src, sc, sc, dst);
    float pg = m.opt.agc ? db_to_gain(m.opt.post_gain_db) : 1.f;
    for (int i = 0; i < kFrames; ++i) { x[i] *= pg; m.s16[i] = x[i] * 32768.f; }
    prob = m.vad->Analyze(webrtc::DeinterleavedView<const float>(m.s16.data(), kFrames, 1));
  } else
#endif
  {
    m.fallback.process(x, kFrames);
    prob = m.fallback.active() ? 1.f : 0.f;
  }
  m.prob = prob;
  // Speech that matters is loud after the gain stage (it settles near -18
  // dBFS); what the canceller leaves of a listener's voice, or of a song with
  // vocals, sits at the noise floor (-50 dBFS or below) and must not count as
  // the owner talking (it would duck the music and light "talking").
  bool speech = prob > 0.5f && block_rms(x, kFrames) > kSpeechFloor;
  // Ducking needs 30 ms of speech to start (a key click is 5 ms), then
  // holds 150 ms across the gaps between syllables.
  m.speech_run = speech ? m.speech_run + 1 : 0;
  if (m.speech_run >= 3 || (speech && m.voice_hold > 0)) m.voice_hold = 15;
  else if (m.voice_hold > 0) --m.voice_hold;
  voice_ = m.voice_hold > 0;

  // Residual echo between the owner's words: a soft, shallow dip while the
  // reference plays and nobody here is speaking. Opens within the block the
  // voice detector fires on; closes over ~100 ms after a 250 ms hold.
  if (m.opt.expander && m.st.apm) {
    if (speech) m.speech_hold = 25; else if (m.speech_hold > 0) --m.speech_hold;
    float target = (m.speech_hold > 0 || !ref_active) ? 1.f : db_to_gain(-kExpanderDepthDb);
    float g0 = m.exp_gain;
    m.exp_gain = target > g0 ? target : g0 + (target - g0) * 0.1f;
    if (g0 != 1.f || m.exp_gain != 1.f)
      for (int i = 0; i < kFrames; ++i) x[i] *= g0 + (m.exp_gain - g0) * float(i) / kFrames;
  }
  // Safety knee: linear to 0.9, then a tanh curve that never reaches full scale.
  for (int i = 0; i < kFrames; ++i) {
    float a = std::fabs(x[i]);
    if (a > 0.9f) x[i] = std::copysign(0.9f + 0.0999f * std::tanh((a - 0.9f) / 0.1f), x[i]);
  }
  float out_rms = block_rms(x, kFrames);
  if (ref_active && prob < 0.2f && m.voice_hold == 0) {
    const double leak = 0.999;  // 1000 blocks = 10 s
    m.echo_in = m.echo_in * leak + double(in_rms) * in_rms;
    m.echo_out = m.echo_out * leak + double(out_rms) * out_rms;
    m.echo_n = m.echo_n * leak + 1;
  }
  if (prob > 0.8f && in_rms > 1e-5f) {
    double a = m.speech_s < 1.0 ? 0.2 : 0.03;
    m.in_lvl = m.speech_s == 0 ? gain_to_db(in_rms) : m.in_lvl + a * (gain_to_db(in_rms) - m.in_lvl);
    m.out_lvl = m.speech_s == 0 ? gain_to_db(out_rms) : m.out_lvl + a * (gain_to_db(out_rms) - m.out_lvl);
    m.speech_s += kFrames / 48000.0;
  }
  m.record(2, x);
  int64_t rev = m.reverse_us.exchange(0);
  float cost = float(now_us() - t0 + rev);

  std::lock_guard lk(m.mu);
  m.cpu_sum += cost;
  m.cpu_n++;
  if (m.cpu_n > 100) m.cpu_max = std::max(m.cpu_max, cost);  // after warm-up (allocation, first adaptation)
  VoiceStats &s = m.st;
  s.blocks++;
  s.reverse_blocks = m.reverse_blocks;
  s.reference_active = ref_active;
  s.speech_prob = prob;
  s.voice = voice_;
  s.in_speech_dbfs = float(m.in_lvl);
  s.out_speech_dbfs = float(m.out_lvl);
  s.gain_db = m.speech_s > 0 ? float(m.out_lvl - m.in_lvl - (m.opt.agc ? m.opt.post_gain_db : 0.f)) : m.opt.initial_gain_db;
  s.speech_seconds = m.speech_s;
  if (m.echo_n > 100 && m.echo_in > 0) {  // at least a second of echo-only audio
    s.echo_in_dbfs = float(10 * std::log10(m.echo_in / m.echo_n + 1e-20));
    s.echo_out_dbfs = float(10 * std::log10(m.echo_out / m.echo_n + 1e-20));
    s.measured_erle_db = s.echo_in_dbfs - s.echo_out_dbfs;
  }
  s.cpu_avg_us = float(m.cpu_sum / double(m.cpu_n));
  s.cpu_max_us = m.cpu_max;
#if AOI_HAVE_APM
  s.aec = m.st.apm && m.opt.aec && echo_on_;
  if (!s.aec) { s.erle_db.reset(); s.erl_db.reset(); s.delay_ms.reset(); s.residual_echo.reset(); s.echo = false; }
  if (m.apm && s.aec && s.blocks % 50 == 0) {
    auto a = m.apm->GetStatistics();
    s.erle_db = a.echo_return_loss_enhancement;
    s.erl_db = a.echo_return_loss;
    s.delay_ms = a.delay_ms;
    s.residual_echo = a.residual_echo_likelihood;
    // Some echo path exists when the reference plays and the mic hears it
    // (a finite, modest echo return loss); headset mics hear almost nothing.
    s.echo = ref_active && a.echo_return_loss && *a.echo_return_loss < 40;
  }
#endif
}

VoiceStats VoiceProcessor::stats() const {
  std::lock_guard lk(impl_->mu);
  return impl_->st;
}

}  // namespace aoi
