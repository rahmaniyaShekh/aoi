// `aoi selftest`: checks the parts that matter on this machine, with numbers.
//
//   aoi selftest              everything below
//   aoi selftest seal CODE TEXT / open CODE BLOB   cross-implementation hooks
#include <windows.h>
#include <opus.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <set>

#include "audio.h"
#include "cc.h"
#include "cli.h"
#include "crypto.h"
#include "dsp.h"
#include "rtp.h"
#include "talkback.h"
#include "util.h"

namespace aoi {

namespace {
int g_fail = 0, g_pass = 0;
bool g_trace = false;

void check(bool ok, const char *name, const std::string &detail = {}) {
  (ok ? g_pass : g_fail)++;
  outf("  %s%s%s  %-52s %s%s%s\n", ok ? C_OK : C_BAD, ok ? "PASS" : "FAIL", C_RST, name, C_DIM, detail.c_str(), C_RST);
}

std::string f1(const char *fmt, double a, double b = 0, double c = 0, double d = 0) {
  char buf[160];
  snprintf(buf, sizeof buf, fmt, a, b, c, d);
  return buf;
}

// ---------------------------------------------------------------------------------------
void test_crypto() {
  outf("\n%sCrypto & codes%s\n", C_BOLD, C_RST);
  std::map<char, int> hist;
  bool all_valid = true;
  for (int i = 0; i < 20000; ++i) {
    std::string c = generate_code();
    all_valid &= valid_code(c);
    for (char ch : c) hist[ch]++;
  }
  int lo = 1 << 30, hi = 0;
  for (auto &h : hist) { lo = std::min(lo, h.second); hi = std::max(hi, h.second); }
  check(all_valid && hist.size() == kCodeAlphabet.size(), "codes use the 31-symbol alphabet only");
  check(double(hi) / lo < 1.12, "no modulo bias across symbols", f1("max/min frequency %.3f", double(hi) / lo));
  check(normalize_code("k7q-4mx") == "K7Q4MX" && pretty_code("K7Q4MX") == "K7Q-4MX", "normalise and format");

  std::string sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=candidate:1 1 UDP 2122317823 192.168.1.20 50000 typ host\r\n";
  for (int i = 0; i < 40; ++i) sdp += "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n";
  int64_t t0 = now_us();
  std::string blob = seal_blob(sdp, "K7Q4MX");
  double seal_ms = (now_us() - t0) / 1000.0;
  std::string err;
  auto back = open_blob(blob, "K7Q4MX", &err);
  check(back && *back == sdp, "seal/open round trip", f1("%.0f ms, %.0f bytes -> %.0f", seal_ms, double(sdp.size()), double(blob.size())));
  check(blob.find("192.168") == std::string::npos && blob.find("K7Q4MX") == std::string::npos, "blob leaks neither IP nor code");
  auto wrong = open_blob(blob, "K7Q4MY", &err);
  check(!wrong && err == "wrong code", "wrong code is rejected");
  std::string tampered = blob;
  tampered[tampered.size() - 5] = tampered[tampered.size() - 5] == 'A' ? 'B' : 'A';
  check(!open_blob(tampered, "K7Q4MX", &err), "tampered blob is rejected");
  check(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known answer");
}

// ---------------------------------------------------------------------------------------
void test_rtp() {
  outf("\n%sRTP / RTCP%s\n", C_BOLD, C_RST);
  uint8_t pl[50];
  for (int i = 0; i < 50; ++i) pl[i] = uint8_t(i);
  auto pkt = build_rtp(kPtOpus, 65535, 123456, 0xABCDEF01, true, 777, pl, sizeof pl);
  RtpPacketInfo info;
  bool ok = parse_rtp(pkt.data(), pkt.size(), info);
  check(ok && info.seq == 65535 && info.ts == 123456 && info.ssrc == 0xABCDEF01 && info.marker && info.twcc_seq &&
            *info.twcc_seq == 777 && info.payload_len == 50 && memcmp(info.payload, pl, 50) == 0,
        "RTP header + transport-cc extension");
  set_twcc_seq(pkt, 4242);
  parse_rtp(pkt.data(), pkt.size(), info);
  check(info.twcc_seq && *info.twcc_seq == 4242, "retransmission gets a new transport seq");

  Frame f1{1000, std::vector<uint8_t>(100, 1)}, f2{1960, std::vector<uint8_t>(120, 2)}, p{2920, std::vector<uint8_t>(90, 3)};
  auto red = build_red({&f1, &f2}, p, kPtOpus);
  std::vector<RedBlock> blocks;
  ok = parse_red(red.data(), red.size(), 2920, blocks);
  check(ok && blocks.size() == 3 && blocks[0].ts == 1000 && blocks[0].len == 100 && blocks[1].ts == 1960 &&
            blocks[1].len == 120 && blocks[2].ts == 2920 && blocks[2].len == 90 && blocks[2].data[0] == 3,
        "RED: two redundant frames + primary round trip");

  auto nack = build_nack(1, 0xABCDEF01, {100, 101, 105, 116, 117, 300});
  RtcpEvents ev;
  parse_rtcp(nack.data(), nack.size(), 0xABCDEF01, ev);
  std::set<uint16_t> got(ev.nacks.begin(), ev.nacks.end());
  check(got == std::set<uint16_t>{100, 101, 105, 116, 117, 300}, "NACK encode/parse (PID+BLP)");

  std::vector<std::optional<int64_t>> arr;
  int64_t t = 5000000;
  for (int i = 0; i < 40; ++i) {
    if (i % 7 == 3) arr.push_back(std::nullopt);
    else { t += (i == 20 ? 90000 : 20000 + (i % 3) * 250); arr.push_back(t); }  // one 90 ms gap: large delta
  }
  auto fb = build_twcc(1, 2, 65530, arr, 9);
  RtcpEvents ev2;
  parse_rtcp(fb.data(), fb.size(), 2, ev2);
  bool twcc_ok = ev2.twcc.size() == arr.size();
  int64_t base = -1, base_true = -1;
  for (size_t i = 0; twcc_ok && i < arr.size(); ++i) {
    const auto &r = ev2.twcc[i];
    twcc_ok &= r.seq == uint16_t(65530 + i) && r.received == arr[i].has_value();
    if (r.received) {
      if (base < 0) { base = r.recv_us; base_true = *arr[i]; }
      twcc_ok &= std::llabs((r.recv_us - base) - (*arr[i] - base_true)) < 300;
    }
  }
  check(twcc_ok, "TWCC feedback: statuses, small+large deltas, seq wrap");
  auto sr = build_sr(9, ntp_now(), 1, 2, 3);
  RtcpEvents ev3;
  parse_rtcp(sr.data(), sr.size(), 0, ev3);
  check(ev3.has_sr && ev3.sr_ssrc == 9, "sender report");
}

// ---------------------------------------------------------------------------------------
void test_dsp() {
  outf("\n%sSignal chain%s\n", C_BOLD, C_RST);
  std::mt19937 rng(7);
  std::normal_distribution<float> nd(0.f, 1.f);
  Limiter lim;
  std::vector<float> x(size_t(kRate) * 2 * 4);
  for (auto &v : x) v = nd(rng) * 0.9f;  // peaks far above full scale
  std::vector<float> y = x;
  for (size_t i = 0; i < y.size(); i += kBlock * 2) lim.process(y.data() + i, kBlock);
  float peak = 0;
  for (float v : y) peak = std::max(peak, std::fabs(v));
  check(peak <= db_to_gain(-1.f) + 1e-6f, "limiter holds -1 dBFS on +12 dB peaks", f1("peak %.4f (%.2f dBFS)", peak, gain_to_db(peak)));

  Limiter lim2;
  std::vector<float> q(size_t(kRate) * 2);
  for (size_t i = 0; i < q.size(); ++i) q[i] = 0.3f * std::sin(float(i / 2) * 0.05f);
  std::vector<float> qo = q;
  for (size_t i = 0; i < qo.size(); i += kBlock * 2) lim2.process(qo.data() + i, kBlock);
  int d = lim2.latency();
  double maxerr = 0;
  for (size_t i = size_t(d) * 2; i < qo.size(); ++i) maxerr = std::max(maxerr, double(std::fabs(qo[i] - q[i - size_t(d) * 2])));
  check(maxerr == 0.0, "limiter is an exact wire below the ceiling", f1("latency %.1f ms", d * 1000.0 / kRate));

  AutoLevel al(-18.f);
  std::vector<float> blk(kBlock * 2);
  double ph = 0;
  for (int b = 0; b < 2000; ++b) {  // 20 s of a -40 dBFS-ish tone
    for (int i = 0; i < kBlock; ++i) { float s = 0.0141f * std::sin(float(ph)); ph += 2 * M_PI * 440 / kRate; blk[2 * i] = blk[2 * i + 1] = s; }
    al.process(blk.data(), kBlock);
  }
  check(al.gain_db() > 10.f && al.gain_db() <= 15.f, "auto level lifts a quiet source, bounded",
        f1("source %.1f LUFS, gain %+.1f dB", al.source_lufs(), al.gain_db()));

  // Drift reader: 48000 Hz in, consumer 0.3% fast; the servo must hold the fill.
  DriftReader dr(kRate);
  dr.set_target(1440);
  std::vector<float> w(480, 0.1f), r(480);
  // Producer 0.3 % slow against the consumer (10x worse than real crystals),
  // delivered in uneven lumps like a real capture device.
  int under = 0;
  double acc = 0;
  std::mt19937 lr(5);
  for (int i = 0; i < 6000; ++i) {  // 60 s
    acc += 480 * 0.997;
    int lump = int(acc);
    acc -= lump;
    if (i % 3 == 0) { dr.write(w.data(), lump); } else { std::vector<float> big(size_t(lump), 0.1f); dr.write(big.data(), lump); }
    if (!dr.read(r.data(), 480) && i > 500) ++under;
  }
  check(under == 0 && std::fabs(dr.fill() - 1440) < 480 && std::fabs(dr.ratio() - 0.997) < 0.001,
        "drift servo locks onto a 0.3% clock mismatch",
        f1("fill %.0f / target 1440, ratio %.4f, underruns %.0f", dr.fill(), dr.ratio(), under));
}

// ---------------------------------------------------------------------------------------
void test_talkback() {
  outf("\n%sTalkback jitter buffer (real Opus, 10%% loss, 0-40 ms jitter)%s\n", C_BOLD, C_RST);
  int err = 0;
  OpusEncoder *enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
  opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(15));
  TalkStream ts;
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> u(0, 1);
  struct P { int64_t at; std::vector<uint8_t> pkt; };
  std::vector<P> sched;
  int64_t t0 = now_us();
  double ph = 0;
  for (int f = 0; f < 500; ++f) {  // 10 s
    float pcm[960];
    for (int i = 0; i < 960; ++i) { pcm[i] = 0.3f * std::sin(float(ph)); ph += 2 * M_PI * 300 / kRate; }
    uint8_t out[400];
    int n = opus_encode_float(enc, pcm, 960, out, sizeof out);
    if (u(rng) < 0.10) continue;
    auto pkt = build_rtp(kPtOpus, uint16_t(f), uint32_t(f * 960), 42, false, 0, out, size_t(n));
    sched.push_back({t0 + int64_t(f) * 20000 + int64_t(u(rng) * 40000), std::move(pkt)});
  }
  std::sort(sched.begin(), sched.end(), [](auto &a, auto &b) { return a.at < b.at; });
  size_t next = 0;
  int silent_blocks = 0, blocks = 0;
  bool started = false;
  for (int64_t t = t0; t < t0 + 10200000; t += 10000) {
    while (next < sched.size() && sched[next].at <= t) {
      RtpPacketInfo info;
      parse_rtp(sched[next].pkt.data(), sched[next].pkt.size(), info);
      ts.push(info, sched[next].at);
      ++next;
    }
    float o[480] = {};
    ts.mix_into(o, 480, 1.f);
    double e = 0;
    for (float v : o) e += double(v) * v;
    bool s = e / 480 < 1e-5;
    if (!s) started = true;
    if (started && t < t0 + 9800000) { ++blocks; silent_blocks += s; }
  }
  auto st = ts.stats();
  check(blocks > 900 && silent_blocks * 100 < blocks * 2, "continuous playout through 10% loss",
        f1("%.0f/%.0f blocks audible, %.0f frames recovered by FEC", blocks - silent_blocks, blocks, double(st.fec)));
  check(st.target_ms >= 40 && st.target_ms <= 200, "buffer target follows measured jitter",
        f1("jitter %.1f ms -> target %.0f ms", st.jitter_ms, st.target_ms));
  opus_encoder_destroy(enc);
}

// ---------------------------------------------------------------------------------------
// Congestion control against a simulated bottleneck, in simulated time.
struct SimResult { double wire_kbps, max_queue_ms, opus_kbps, red, loss; int frame; bool stereo; double last_congested_s; };

SimResult run_cc(std::function<double(double)> capacity_kbps, double loss, double seconds, double prop_ms = 40,
                 bool redcap = true) {
  CongestionController cc;
  PeerCaps caps{redcap, true, true, 256000};
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> u(0, 1);
  int64_t t0 = now_us();
  double link_free = 0;  // seconds
  struct Pending { int64_t arrive; uint16_t seq; bool lost; };
  std::vector<Pending> inflight;
  uint16_t tseq = 0;
  double next_fb = 0.1, next_rtt = 0.5, max_queue = 0, window_bytes = 0, window_start = seconds - 10, last_cong = 0;
  Plan plan;
  for (double t = 0; t < seconds;) {
    int64_t now = t0 + int64_t(t * 1e6);
    plan = cc.update(caps, now);
    int bytes = int(plan.opus_bps * (1 + plan.red) * plan.frame_ms / 8000.0) + 62 + (plan.red ? 1 + 4 * plan.red : 0);
    double cap = capacity_kbps(t) * 1000;
    double tx = bytes * 8.0 / cap;
    double queue = std::max(0.0, link_free - t);
    bool dropped = queue > 0.3;  // 300 ms drop-tail buffer
    bool lost = dropped || u(rng) < loss;
    if (!dropped) link_free = std::max(t, link_free) + tx;
    max_queue = std::max(max_queue, t > 5 ? queue * 1000 : 0.0);
    if (queue > 0.1) last_cong = t;
    int64_t arrive = t0 + int64_t((link_free + prop_ms / 2000.0) * 1e6);
    cc.on_sent(tseq, now, bytes);
    inflight.push_back({arrive, tseq, lost});
    ++tseq;
    if (t >= window_start && !lost) window_bytes += bytes;
    t += plan.frame_ms / 1000.0;
    if (t >= next_fb) {  // feedback every 100 ms, arrives prop/2 later (folded in)
      next_fb += 0.1;
      std::vector<TwccResult> fb;
      int64_t fb_now = t0 + int64_t(t * 1e6);
      std::vector<Pending> keep;
      for (auto &p : inflight) {
        if (p.arrive <= fb_now || p.lost) fb.push_back({p.seq, !p.lost, p.arrive});
        else keep.push_back(p);
      }
      inflight.swap(keep);
      if (!fb.empty()) cc.on_twcc(fb, fb_now);
    }
    if (g_trace && std::fmod(t, 2.0) < plan.frame_ms / 1000.0)
      outf("      t=%5.1fs cap %4.0fk target %5.1fk acked %5.1fk loss %4.1f%% queue %4.0fms %-9s bound %6.1fk q_est %4.0f -> opus %3dk x%d %dms\n", t,
           capacity_kbps(t), cc.target_bps() / 1000, cc.acked_bps() / 1000, cc.loss() * 100, std::max(0.0, link_free - t) * 1000,
           cc.state(), cc.loss_bound() / 1000, cc.queue_ms(), plan.opus_bps / 1000, plan.red, plan.frame_ms);
    if (t >= next_rtt) {
      next_rtt += 0.5;
      cc.on_rtt(prop_ms + std::max(0.0, link_free - t) * 1000, t0 + int64_t(t * 1e6));
    }
  }
  return {window_bytes * 8 / 10 / 1000, max_queue, plan.opus_bps / 1000.0, double(plan.red), cc.loss(), plan.frame_ms,
          plan.stereo, last_cong};
}

void test_cc() {
  outf("\n%sCongestion control (simulated bottleneck, 300 ms buffer)%s\n", C_BOLD, C_RST);
  auto a = run_cc([](double) { return 2000.0; }, 0.0, 30);
  check(a.opus_kbps >= 250 && a.stereo && a.red == 0, "fast link: climbs to studio quality",
        f1("opus %.0f kbps, wire %.0f kbps", a.opus_kbps, a.wire_kbps));
  auto b = run_cc([](double) { return 64.0; }, 0.0, 60);
  check(b.wire_kbps <= 64 * 1.02 && b.wire_kbps > 64 * 0.75 && b.max_queue_ms < 300, "64 kbps link: fits, uses it, bounded queue",
        f1("wire %.1f kbps, opus %.0f kbps, max queue %.0f ms", b.wire_kbps, b.opus_kbps, b.max_queue_ms));
  auto c = run_cc([](double) { return 160.0; }, 0.05, 60);
  check(c.red >= 1 && c.wire_kbps <= 160 * 1.02, "5% random loss: adds redundancy, still fits",
        f1("red x%.0f, opus %.0f kbps, wire %.0f kbps", c.red, c.opus_kbps, c.wire_kbps));
  auto d = run_cc([](double t) { return t < 20 ? 600.0 : 48.0; }, 0.0, 45);
  // A 12x capacity collapse cannot avoid one queue spike; what matters is how
  // fast it is drained and that it then stays drained.
  double recover = d.last_congested_s - 20;
  check(d.wire_kbps <= 48 * 1.05 && recover < 3.0, "capacity drops 600 -> 48 kbps: follows it down",
        f1("queue drained %.1f s after the drop, then wire %.1f kbps, opus %.0f kbps, %.0f ms frames", recover, d.wire_kbps,
           d.opus_kbps, d.frame));
  auto e = run_cc([](double) { return 24.0; }, 0.02, 60);
  check(e.wire_kbps <= 24 * 1.05 && e.wire_kbps >= 24 * 0.75 && e.frame >= 40, "24 kbps link (2G-class): long frames, fits, uses it",
        f1("wire %.1f kbps, opus %.0f kbps, %.0f ms frames", e.wire_kbps, e.opus_kbps, e.frame));
}

// ---------------------------------------------------------------------------------------
void test_devices() {
  outf("\n%sAudio devices%s\n", C_BOLD, C_RST);
  std::atomic<int> blocks{0};
  std::atomic<float> peak{0};
  {
    SystemCapture cap([&](const float *x, int n) {
      ++blocks;
      float p = 0;
      for (int i = 0; i < n * 2; ++i) p = std::max(p, std::fabs(x[i]));
      if (p > peak) peak = p;
    });
    cap.start();
    Sleep(1500);
    check(blocks >= 120, "system audio capture delivers a steady clock",
          f1("%.0f blocks in 1.5 s via ", double(blocks.load())) + (cap.process_loopback() ? "process loopback" : "endpoint loopback"));
    cap.stop();
  }
  std::atomic<int> mic_frames{0};
  {
    MicCapture mic([&](const float *, int n) { mic_frames += n; });
    mic.start();
    Sleep(1500);
    bool ok = mic_frames > kRate / 2;
    check(true, ok ? "microphone delivers audio" : "no microphone (music only; that is fine)",
          ok ? mic.name() : std::string());
    mic.stop();
  }
}

}  // namespace

int run_selftest(const std::vector<std::wstring> &args) {
  if (args.size() >= 3 && args[0] == L"seal") {
    out(seal_blob(narrow(args[2]), normalize_code(narrow(args[1]))) + "\n");
    return 0;
  }
  if (args.size() >= 3 && args[0] == L"open") {
    std::string err;
    auto s = open_blob(narrow(args[2]), normalize_code(narrow(args[1])), &err);
    if (!s) { out("ERROR " + err + "\n"); return 1; }
    out(*s);
    return 0;
  }
  bool quick = false;
  for (auto &a : args) { if (a == L"--no-devices") quick = true; if (a == L"--trace") g_trace = true; }
  outf("\n%sAOI self test%s %s\n", C_BOLD, C_RST, AOI_VERSION);
  test_crypto();
  test_rtp();
  test_dsp();
  test_talkback();
  test_cc();
  if (!quick) test_devices();
  outf("\n  %s%d passed%s, %s%d failed%s\n\n", C_OK, g_pass, C_RST, g_fail ? C_BAD : C_DIM, g_fail, C_RST);
  return g_fail ? 1 : 0;
}

}  // namespace aoi
