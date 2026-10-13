// `aoi selftest`: voice processing (echo, noise, level, voice activity, CPU),
// the device policy and the platform boundary, with real numbers.
//
// The signals are synthetic but built to behave like the real thing: speech
// is a glottal source through moving formants in syllables and phrases, the
// music is plucked notes at random (never an exact repeat, as no song is)
// with bass and irregular drums, and the room is a fan plus keyboard clicks.
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "boundary_report.h"
#include "cli.h"
#include "devices.h"
#include "dsp.h"
#include "selftest.h"
#include "voice.h"

namespace aoi::st {

namespace {
constexpr int R = kRate, B = kBlock;

double db10(double p) { return 10 * std::log10(p + 1e-20); }

Biquad bandpass(double fc, double q) {
  double w = 2 * M_PI * fc / R, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
  Biquad b;
  b.b0 = al / a0; b.b1 = 0; b.b2 = -al / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0;
  return b;
}

// Speech-like: syllables of a glottal source through gliding formants,
// fricative onsets, pauses between words and phrases. `active` marks speech.
std::vector<float> speech(double seconds, unsigned seed, double f0base, std::vector<char> *active = nullptr) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<double> u(0, 1);
  std::normal_distribution<double> n(0, 1);
  size_t N = size_t(seconds * R);
  std::vector<float> y(N, 0.f);
  if (active) active->assign(N, 0);
  static const double V[][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240},
                                {530, 1840, 2480}, {570, 840, 2410}, {440, 1020, 2240}};
  size_t t = size_t(0.3 * R);
  double phase = 0;
  while (t < N) {
    int syl = 2 + int(u(g) * 5);
    for (int s = 0; s < syl && t < N; ++s) {
      size_t len = size_t((0.12 + u(g) * 0.18) * R);
      const double *va = V[int(u(g) * 6)], *vb = V[int(u(g) * 6)];
      Biquad f[3];
      double f0 = f0base * (0.85 + 0.3 * u(g)), glide = (u(g) - 0.5) * 0.4;
      bool fric = u(g) < 0.3;
      Biquad fh = Biquad::highpass(3500, 0.7);
      for (size_t i = 0; i < len && t + i < N; ++i) {
        double x = double(i) / len;
        double env = std::pow(std::sin(M_PI * x), 0.6);
        if (i % 96 == 0)
          for (int k = 0; k < 3; ++k) {
            Biquad b = bandpass(va[k] + (vb[k] - va[k]) * x, 6 + 2 * k);
            b.z1 = f[k].z1; b.z2 = f[k].z2;
            f[k] = b;
          }
        phase += f0 * (1 + glide * x) / R;
        if (phase > 1) phase -= 1;
        double src = (1 - 2 * phase) + 0.02 * n(g);
        double v = f[0].run(src) + f[1].run(src) * 0.6 + f[2].run(src) * 0.3;
        double fr = fric && x < 0.25 ? fh.run(n(g)) * 0.25 * (1 - x / 0.25) : 0;
        y[t + i] = float((v * env + fr) * 0.5);
        if (active) (*active)[t + i] = 1;
      }
      t += len + size_t((0.03 + u(g) * 0.08) * R);
    }
    t += size_t((0.25 + u(g) * 0.6) * R);
  }
  return y;
}

std::vector<float> music(double seconds, unsigned seed) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<double> u(0, 1);
  std::normal_distribution<double> n(0, 1);
  size_t N = size_t(seconds * R);
  std::vector<double> y(N, 0.0);
  static const int scale[] = {0, 2, 4, 5, 7, 9, 11};
  auto note = [&](double t0, double f, double dur, double amp, int harm) {
    size_t s0 = size_t(t0 * R), len = size_t(dur * R);
    double det = 1 + (u(g) - 0.5) * 0.004, ph[24];
    for (double &p : ph) p = u(g) * 2 * M_PI;
    for (size_t i = 0; i < len && s0 + i < N; ++i) {
      double t = double(i) / R, v = 0;
      for (int h = 1; h <= harm && f * h < 9000; ++h) v += std::sin(2 * M_PI * f * det * h * t + ph[h - 1]) / h * std::exp(-t * (2 + h * 0.6));
      double att = t < 0.02 ? n(g) * 0.3 * (1 - t / 0.02) : 0;
      y[s0 + i] += (v + att) * std::min(1.0, t * 300) * amp;
    }
  };
  for (double t = 0; t < seconds; t += 0.08 + u(g) * 0.35)
    note(t, 261.6 * std::pow(2.0, (scale[int(u(g) * 7)] + 12 * int(u(g) * 2)) / 12.0), 0.3 + u(g) * 1.2,
         0.05 + u(g) * 0.08, 20);
  for (double t = 0; t < seconds; t += 0.25 + 0.25 * int(u(g) * 3))
    note(t, 65.4 * std::pow(2.0, scale[int(u(g) * 7)] / 12.0), 0.4, 0.18, 12);
  Biquad kick = Biquad::lowpass(150), hat = Biquad::highpass(6000), snare = bandpass(1800, 0.8);
  double nk = 0, nh = 0.1, ns = 0.3;
  int ck = 0, chh = 0, cs = 0;
  for (size_t i = 0; i < N; ++i) {
    double t = double(i) / R;
    if (t >= nk) { ck = 4000; nk = t + 0.25 * (1 + int(u(g) * 3)); }
    if (t >= nh) { chh = 1200; nh = t + 0.125 * (1 + int(u(g) * 2)); }
    if (t >= ns) { cs = 5000; ns = t + 0.5 * (1 + int(u(g) * 2)); }
    double k = ck > 0 ? std::sin(2 * M_PI * (50 + ck * 0.02) * t) * (ck / 4000.0) : 0;
    if (ck) --ck;
    double hh = hat.run(n(g)) * (chh / 1200.0) * 0.25;
    if (chh) --chh;
    double sn = snare.run(n(g)) * (cs / 5000.0) * 0.35;
    if (cs) --cs;
    y[i] += kick.run(k) * 0.6 + hh + sn;
  }
  std::vector<float> o(N);
  for (size_t i = 0; i < N; ++i) o[i] = float(std::tanh(y[i] * 0.8) * 0.7);
  return o;
}

// A laptop fan (low-passed noise and motor hum) plus keyboard clicks.
std::vector<float> room_noise(double seconds, unsigned seed, double level) {
  std::mt19937 g(seed);
  std::normal_distribution<double> n(0, 1);
  std::uniform_real_distribution<double> u(0, 1);
  size_t N = size_t(seconds * R);
  std::vector<float> y(N);
  Biquad lp1 = Biquad::lowpass(900), lp2 = Biquad::lowpass(2500), click = bandpass(3000, 2);
  double next_click = 0.2;
  int clk = 0;
  for (size_t i = 0; i < N; ++i) {
    double t = double(i) / R;
    double fan = lp1.run(n(g)) * 1.2 + lp2.run(n(g)) * 0.25 + 0.15 * std::sin(2 * M_PI * 120 * t) +
                 0.08 * std::sin(2 * M_PI * 240 * t);
    if (t > next_click) { clk = 240; next_click = t + 0.08 + u(g) * 0.25; }
    double k = 0;
    if (clk > 0) { k = click.run(n(g)) * 3.0 * (clk / 240.0); --clk; }
    else click.run(0);
    y[i] = float((fan + k) * level);
  }
  return y;
}

// The loudspeaker-to-mic path of a laptop: delay, a small speaker's band
// limits, a few reflections.
std::vector<float> echo_path(const std::vector<float> &ref, int delay_ms, float gain) {
  size_t N = ref.size(), D = size_t(delay_ms) * R / 1000;
  std::vector<float> e(N, 0.f);
  Biquad lp = Biquad::lowpass(5000), hp = Biquad::highpass(250);
  for (size_t i = 0; i < N; ++i) {
    double v = 0;
    if (i >= D) v += ref[i - D];
    if (i >= D + 240) v += 0.35 * ref[i - D - 240];
    if (i >= D + 700) v -= 0.2 * ref[i - D - 700];
    if (i >= D + 1500) v += 0.1 * ref[i - D - 1500];
    e[i] = float(hp.run(lp.run(v)) * gain);
  }
  return e;
}

// Best alignment of `out` against `ref` over [from, from+len): lag and normalised correlation.
int best_lag(const std::vector<float> &ref, const std::vector<float> &out, size_t from, size_t len, int maxlag,
             double *corr) {
  int best = 0;
  double bv = -2;
  for (int L = 0; L < maxlag; L += 2) {
    double c = 0, ea = 0, eb = 0;
    for (size_t i = from; i < from + len; i += 2) {
      c += double(ref[i]) * out[i + size_t(L)];
      ea += double(ref[i]) * ref[i];
      eb += double(out[i + size_t(L)]) * out[i + size_t(L)];
    }
    double v = c / std::sqrt(ea * eb + 1e-20);
    if (v > bv) { bv = v; best = L; }
  }
  *corr = bv;
  return best;
}

struct AecResult { double erle, seg_mean, seg_min, near_db, corr; std::optional<double> apm_erle; };

// `shipped`: canceller + noise suppression + residual-echo expander, as it runs
// (only the gain stage is off, since a gain would hide what was removed);
// otherwise AEC3 + noise suppression alone.

AecResult run_aec(int delay_ms, bool shipped) {
  const double T = 30;
  // Generated once for all six runs.
  static const std::vector<float> near = speech(T, 3, 110), noise = room_noise(T, 4, 0.0004);
  static const std::vector<float> ref = [&] {
    auto mus = music(T, 1), far = speech(T, 2, 180);
    std::vector<float> r(mus.size());
    for (size_t i = 0; i < r.size(); ++i) r[i] = 0.7f * mus[i] + 0.5f * far[i];  // the music + a listener talking back
    return r;
  }();
  size_t N = ref.size();
  auto echo = echo_path(ref, delay_ms, 0.3f);
  VoiceOptions o;
  o.agc = false;  // a gain stage would hide what the canceller did
  o.expander = shipped;
  VoiceProcessor vp(o);
  vp.set_delay_ms(60);  // a rough device-latency hint, as on a real PC; AEC3 finds the rest
  std::vector<float> out(N, 0.f), nearv(N, 0.f), blk(B);
  double pin = 0, pout = 0, win_in = 0, win_out = 0;
  std::vector<double> seg;
  std::optional<double> apm_erle;
  for (size_t at = 0; at + B <= N; at += B) {
    bool dt = at >= size_t(22 * R);  // double talk from 22 s
    for (int i = 0; i < B; ++i) {
      nearv[at + i] = dt ? 0.5f * near[at + i] : 0.f;
      blk[i] = echo[at + i] + noise[at + i] + nearv[at + i];
    }
    vp.reverse(ref.data() + at);
    vp.process(blk.data());
    std::copy(blk.begin(), blk.end(), out.begin() + long(at));
    if (at >= size_t(12 * R) && at < size_t(22 * R)) {
      for (int i = 0; i < B; ++i) {
        double d = echo[at + i] + noise[at + i];
        pin += d * d;
        win_in += d * d;
        pout += double(blk[i]) * blk[i];
        win_out += double(blk[i]) * blk[i];
      }
      if ((at / B) % 50 == 49) { seg.push_back(db10(win_in) - db10(win_out)); win_in = win_out = 0; }  // 0.5 s windows
    }
    if (at == size_t(21 * R)) apm_erle = vp.stats().erle_db;
  }
  AecResult r;
  r.erle = db10(pin) - db10(pout);
  r.seg_mean = 0;
  for (double e : seg) r.seg_mean += e / double(seg.size());
  r.seg_min = *std::min_element(seg.begin(), seg.end());
  double c;
  size_t from = 24 * R, len = 4 * R;
  int L = best_lag(nearv, out, from, len, 1100 + VoiceProcessor::kCaptureLeadMs * 48, &c);
  double es = 0, eo = 0;
  for (size_t i = from; i < from + len; ++i) { es += double(nearv[i]) * nearv[i]; eo += double(out[i + size_t(L)]) * out[i + size_t(L)]; }
  r.near_db = db10(eo) - db10(es);
  r.corr = c;
  r.apm_erle = apm_erle;
  return r;
}

}  // namespace

std::vector<float> synth_speech(double seconds, unsigned seed, double f0) { return speech(seconds, seed, f0); }
std::vector<float> synth_music(double seconds, unsigned seed) { return music(seconds, seed); }

void test_voice() {
  outf("\n%sVoice processing (%s)%s\n", C_BOLD,
       VoiceProcessor::apm_available() ? "WebRTC APM 2.1: AEC3, noise suppression, AGC2" : "basic chain", C_RST);
  if (!VoiceProcessor::apm_available()) {
    check(false, "WebRTC audio processing is built in", "rebuild after deps.ps1");
    return;
  }
  // ---- echo cancellation ----
  double worst = 1e9, worst_win = 1e9, near_lo = 1e9, near_hi = -1e9, corr_lo = 1;
  std::string per;
  for (int d : {40, 80, 120}) {
    AecResult r = run_aec(d, true);
    worst = std::min(worst, r.erle);
    worst_win = std::min(worst_win, r.seg_min);
    near_lo = std::min(near_lo, r.near_db);
    near_hi = std::max(near_hi, r.near_db);
    corr_lo = std::min(corr_lo, r.corr);
    per += (per.empty() ? "" : " / ") + f1("%.1f", r.erle);
  }
  check(worst >= 25, "echo: music + talkback through a 40-120 ms room path",
        "ERLE at 40/80/120 ms: " + per + f1(" dB (worst 0.5 s: %.0f dB)", worst_win));
  check(near_lo > -3 && near_hi < 2 && corr_lo > 0.7, "double talk: the owner's voice survives the canceller",
        f1("near-end level %+.1f..%+.1f dB, correlation %.2f", near_lo, near_hi, corr_lo));

  // ---- noise suppression ----
  {
    std::vector<char> act;
    auto sp = speech(20, 5, 130, &act);
    auto nz = room_noise(20, 6, 0.02);
    VoiceOptions o;
    o.aec = false;
    o.agc = false;
    o.expander = false;
    VoiceProcessor vp(o);
    std::vector<float> blk(B);
    double nin = 0, nout = 0, sin_ = 0, sout = 0;
    int n_noise = 0, n_speech = 0;
    for (size_t at = 0; at + B <= sp.size(); at += B) {
      int a = 0;
      for (int i = 0; i < B; ++i) { blk[i] = 0.3f * sp[at + i] + nz[at + i]; a += act[at + i]; }
      vp.process(blk.data());
      if (at < size_t(3 * R)) continue;  // let it learn the room
      double pi = 0, po = 0, ps = 0;
      for (int i = 0; i < B; ++i) {
        pi += double(nz[at + i]) * nz[at + i];
        po += double(blk[i]) * blk[i];
        ps += 0.09 * double(sp[at + i]) * sp[at + i];
      }
      if (a == 0) { nin += pi; nout += po; ++n_noise; }
      else if (a == B) { sin_ += ps; sout += po; ++n_speech; }
    }
    double red = db10(nin / n_noise) - db10(nout / n_noise);
    // Speech in the output = what speech blocks carry minus the residual noise.
    double sp_out = std::max(sout / n_speech - nout / n_noise, 1e-12);
    double change = db10(sp_out) - db10(sin_ / n_speech);
    check(red >= 10 && std::fabs(change) <= 2, "noise: fan + keyboard down, speech kept",
          f1("noise -%.1f dB, speech %+.1f dB", red, change));
  }

  // ---- level ----
  {
    std::vector<char> act;
    auto sp = speech(14, 7, 140, &act);
    double rms = 0;
    size_t na = 0;
    for (size_t i = 0; i < sp.size(); ++i) if (act[i]) { rms += double(sp[i]) * sp[i]; ++na; }
    double g = db_to_gain(-40.f) / std::sqrt(rms / double(na));
    VoiceOptions o;
    o.aec = false;
    VoiceProcessor vp(o);  // a microphone it has never met: starts at the default gain
    std::vector<float> blk(B);
    double w15 = 0, n15 = 0, set = 0, nset = 0, loud = 0, nloud = 0, peak = 0;
    for (size_t at = 0; at + B <= sp.size(); at += B) {
      bool louder = at >= size_t(8 * R);  // the same voice 18 dB louder
      int a = 0;
      for (int i = 0; i < B; ++i) { blk[i] = float(sp[at + i] * g * (louder ? 7.943 : 1.0)); a += act[at + i]; }
      vp.process(blk.data());
      double p = 0;
      for (int i = 0; i < B; ++i) { p += double(blk[i]) * blk[i]; if (louder) peak = std::max(peak, double(std::fabs(blk[i]))); }
      if (a != B) continue;
      double t = double(at) / R;
      if (t >= 1.5 && t < 2.0) { w15 += p; n15 += B; }
      if (t >= 3 && t < 8) { set += p; nset += B; }
      if (t >= 12) { loud += p; nloud += B; }
    }
    double l15 = db10(w15 / std::max(n15, 1.0)), lset = db10(set / std::max(nset, 1.0));
    check(std::fabs(l15 + 18) <= 3 && std::fabs(lset + 18) <= 3, "level: a -40 dBFS voice reaches -18 dBFS",
          f1("%.1f dBFS at 1.5-2 s, %.1f dBFS settled", l15, lset));
    check(peak < 0.999, "level: the same voice 18 dB louder does not clip",
          f1("peak %.1f dBFS, speech %.1f dBFS", gain_to_db(float(peak)), db10(loud / std::max(nloud, 1.0))));
  }

  // ---- voice activity (drives ducking) ----
  {
    std::vector<char> act;
    auto sp = speech(12, 8, 120, &act);
    auto mus = music(12, 9);
    auto mus_echo = echo_path(mus, 70, 0.3f);  // the shared music, out of the laptop speakers
    auto nz = room_noise(12, 10, 0.05);
    auto frac = [&](int which) {
      VoiceProcessor vp;
      std::vector<float> blk(B);
      int on = 0, n = 0;
      for (size_t at = 0; at + B <= sp.size(); at += B) {
        int a = 0;
        for (int i = 0; i < B; ++i) {
          blk[i] = which == 0 ? 0.1f * sp[at + i] : which == 1 ? mus_echo[at + i] : nz[at + i];
          a += act[at + i];
        }
        if (which == 1) vp.reverse(mus.data() + at);
        vp.process(blk.data());
        if (at < size_t(2 * R)) continue;  // the canceller's first seconds of convergence
        if (which == 0 && a != B) continue;
        ++n;
        on += vp.voice_active();
      }
      return 100.0 * on / std::max(n, 1);
    };
    double fs = frac(0), fm = frac(1), fk = frac(2);
    check(fs >= 80 && fm <= 5 && fk <= 5, "ducking follows voices, not music or keys",
          f1("voice detected: speech %.0f%%, music from the speakers %.0f%%, fan+keyboard %.0f%%", fs, fm, fk));
  }

  // ---- any device rate (Bluetooth hands-free runs at 8 or 16 kHz, many USB mics at 44.1) ----
  {
    double worst = 1e9;
    std::string per;
    for (int rate : {8000, 16000, 44100, 96000}) {
      double f = 1000;
      std::vector<float> in(size_t(rate) * 2), out;
      for (size_t i = 0; i < in.size(); ++i) in[i] = 0.5f * float(std::sin(2 * M_PI * f * double(i) / rate));
      Resampler rs(1, rate, R);
      for (size_t at = 0; at < in.size(); at += 441) rs.process(in.data() + at, int(std::min<size_t>(441, in.size() - at)), out);
      // Fit a 1 kHz sinusoid over the steady middle, measure what is left.
      size_t a0 = out.size() / 4, n = out.size() / 2;
      double ci = 0, cq = 0;
      for (size_t i = a0; i < a0 + n; ++i) {
        double w = 2 * M_PI * f * double(i) / R;
        ci += out[i] * std::cos(w);
        cq += out[i] * std::sin(w);
      }
      ci *= 2.0 / double(n);
      cq *= 2.0 / double(n);
      double sig = 0, err = 0;
      for (size_t i = a0; i < a0 + n; ++i) {
        double w = 2 * M_PI * f * double(i) / R, fit = ci * std::cos(w) + cq * std::sin(w);
        sig += fit * fit;
        err += (out[i] - fit) * (out[i] - fit);
      }
      double snr = db10(sig) - db10(err);
      worst = std::min(worst, snr);
      per += (per.empty() ? "" : ", ") + f1("%.1f kHz", rate / 1000.0) + f1(" %.0f dB", snr);
    }
    check(worst > 60, "any device rate converts cleanly to 48 kHz", "SNR " + per);
  }

  // ---- cost ----
  {
    auto mus = music(10, 11);
    auto sp = speech(10, 12, 130);
    VoiceProcessor vp;  // the full chain, as it runs
    std::vector<float> blk(B);
    for (size_t at = 0; at + B <= mus.size(); at += B) {
      for (int i = 0; i < B; ++i) blk[i] = 0.05f * mus[at + i] + 0.1f * sp[at + i];
      vp.reverse(mus.data() + at);
      vp.process(blk.data());
    }
    VoiceStats s = vp.stats();
    check(s.cpu_avg_us < 1000, "processing cost per 10 ms block (full chain)",
          f1("%.0f us average, %.0f us worst (budget 1000 us)", s.cpu_avg_us, s.cpu_max_us));
  }
}

// ---------------------------------------------------------------------------------------
namespace {
DeviceInfo dev(const char *id, const char *name, DeviceKind k, FormFactor f, DeviceBus b, const char *container,
               bool def = false, bool comms = false, bool hfp = false) {
  DeviceInfo d;
  d.id = id; d.name = name; d.kind = k; d.form = f; d.bus = b; d.container = container;
  d.builtin_container = std::string(container) == "{pc}";
  d.default_console = def; d.default_comms = comms; d.bluetooth_hfp = hfp;
  return d;
}
}  // namespace

void test_policy() {
  outf("\n%sDevice choice (fake device lists)%s\n", C_BOLD, C_RST);
  using K = DeviceKind;
  using F = FormFactor;
  using Bu = DeviceBus;
  DeviceInfo array = dev("arr", "Microphone Array (Intel)", K::Capture, F::Microphone, Bu::BuiltIn, "{pc}", true);
  DeviceInfo buds = dev("bud", "Headset (CMF Buds 2a)", K::Capture, F::Headset, Bu::Bluetooth, "{buds}", false, true, true);
  DeviceInfo usb = dev("usb", "Headset Microphone (USB)", K::Capture, F::Headset, Bu::Usb, "{usb}");
  DeviceInfo cam = dev("cam", "Webcam Microphone", K::Capture, F::Microphone, Bu::Usb, "{cam}", false, true);
  DeviceInfo spk = dev("spk", "Speakers (Realtek)", K::Render, F::Speakers, Bu::BuiltIn, "{pc}", true);
  DeviceInfo budsout = dev("budo", "Headphones (CMF Buds 2a)", K::Render, F::Headphones, Bu::Bluetooth, "{buds}", false, true);
  DeviceInfo usbout = dev("usbo", "Headset Earphone (USB)", K::Render, F::Headset, Bu::Usb, "{usb}");
  DevicePrefs autop;
  auto name = [](const DeviceChoice &c) { return c.device ? c.device->id : std::string("none"); };

  bool order = name(choose_mic({array, usb, buds}, autop)) == "usb" &&
               choose_mic({array, usb, buds}, autop).reason == "auto: USB headset" &&
               name(choose_mic({array, buds}, autop)) == "arr" &&
               name(choose_mic({cam, buds}, autop)) == "cam" &&
               choose_mic({cam, buds}, autop).reason == "auto: Windows communications device" &&
               name(choose_mic({buds}, autop)) == "bud" && !choose_mic({}, autop).device;
  check(order, "mic order: headset, default, comms; classic Bluetooth last",
        "USB headset > array (default) > webcam (comms) > buds (call quality) only alone");

  DevicePrefs pick = autop;
  pick.mic_id = "arr";
  pick.mic_name = array.name;
  DevicePrefs gone = autop;
  gone.mic_id = "old";
  gone.mic_name = "Yeti";
  DeviceChoice g = choose_mic({array, usb}, gone);
  check(name(choose_mic({array, usb}, pick)) == "arr" && name(g) == "usb" && g.chosen_missing,
        "explicit choice wins; a missing one falls back to auto", g.reason);

  std::vector<DeviceInfo> outs = {spk, budsout, usbout};
  DeviceChoice sb = choose_speaker(outs, buds, autop), su = choose_speaker(outs, usb, autop),
               sa = choose_speaker(outs, array, autop);
  DevicePrefs spick = autop;
  spick.speaker_id = "spk";
  check(name(sb) == "budo" && name(su) == "usbo" && name(sa) == "spk" &&
            name(choose_speaker(outs, buds, spick)) == "spk",
        "talkback output: same device as the mic, else default",
        "buds mic -> buds, USB mic -> USB, laptop mic -> default (speakers), choice wins");

  DevicePrefs p = autop;
  std::string m1, m2, m3, m4;
  std::vector<DeviceInfo> caps = {buds, array, usb};
  bool a1 = apply_device_choice(p, K::Capture, caps, "2", &m1) && p.mic_id == "arr";
  bool a2 = apply_device_choice(p, K::Capture, caps, "laptop", &m2) && p.mic_id == "arr";
  bool a3 = apply_device_choice(p, K::Capture, caps, "buds", &m3) && p.mic_id == "bud";
  bool a4 = !apply_device_choice(p, K::Capture, caps, "headset", &m4) && p.mic_id == "bud";  // ambiguous: kept
  bool a5 = apply_device_choice(p, K::Capture, caps, "off", nullptr) && !p.mic_on &&
            apply_device_choice(p, K::Capture, caps, "auto", nullptr) && p.mic_id.empty() && p.mic_on;
  check(a1 && a2 && a3 && a4 && a5, "aoi mic <n|name|laptop|auto|off> resolves", m4);
  check(!bluetooth_note(buds).empty() && bluetooth_note(array).empty(), "Bluetooth call-quality note only for HFP mics",
        bluetooth_note(buds).substr(0, 48) + "...");
}

void test_boundary() {
  outf("\n%sPlatform boundary%s\n", C_BOLD, C_RST);
  check(AOI_BOUNDARY_VIOLATIONS == 0, "core includes no OS header (checked at build)",
        f1("%.0f core files, %.0f includes scanned", AOI_BOUNDARY_FILES, AOI_BOUNDARY_INCLUDES));
}

}  // namespace aoi::st
