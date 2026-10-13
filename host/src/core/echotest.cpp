// `aoi echotest`: measures echo on this PC's real speakers and microphone.
//
// Runs the real engine offline (nothing is published) for about 35 s:
//   0-4 s    quiet: the room's own level
//   4-14 s   a listener talking back, played through the talkback output
//   14-24 s  music from another app (the speakers, via the system mix)
//   24-34 s  both at once
// and reports, per phase, how loud the echo is at the raw microphone and what
// is left of it after voice processing: what listeners would hear back.
// `--dump <file.wav>` also writes raw (left) and processed (right) mic audio.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

#include "cli.h"
#include "engine.h"
#include "selftest.h"
#include "util.h"

namespace aoi {

namespace {

void normalize(std::vector<float> &x, float rms_db) {
  double s = 0;
  size_t n = 0;
  for (float v : x) if (std::fabs(v) > 1e-4f) { s += double(v) * v; ++n; }
  if (!n) return;
  float g = db_to_gain(rms_db) / float(std::sqrt(s / double(n)));
  for (float &v : x) v *= g;
}

bool write_wav(const std::filesystem::path &p, const std::vector<float> &x, int channels) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
  std::vector<int16_t> pcm(x.size());
  for (size_t i = 0; i < x.size(); ++i) pcm[i] = int16_t(std::lrint(std::clamp(x[i], -1.f, 1.f) * 32767.f));
  uint32_t bytes = uint32_t(pcm.size() * 2);
  f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(uint16_t(channels));
  u32(kRate); u32(kRate * 2 * uint32_t(channels)); u16(uint16_t(2 * channels)); u16(16);
  f.write("data", 4); u32(bytes);
  f.write(reinterpret_cast<const char *>(pcm.data()), std::streamsize(bytes));
  return bool(f);
}

struct Phase {
  const char *name;
  double from, to;
  double raw = 0, out = 0, n = 0, voice = 0;
};


// `aoi echotest --timing <dir>`: no microphone. Records the echo reference as
// fed to the canceller and what the talkback device really plays (its
// loopback), both with arrival times, for 14 s: a voice (talkback) for 6 s,
// then music from another app. Quiet (-35 dBFS). Writes ref.bin and out.bin
// (int64 time, int32 frames, float samples) for offline analysis.
int echo_timing(const std::filesystem::path &dir) {
  auto talk = st::synth_speech(6, 21, 190);
  normalize(talk, -35.f);
  auto music = st::synth_music(8, 22);
  normalize(music, -35.f);
  std::vector<float> stereo(music.size() * 2);
  for (size_t i = 0; i < music.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = music[i];
  auto wav = dir / "timing-music.wav";
  write_wav(wav, stereo, 2);
  std::ofstream fr(dir / "ref.bin", std::ios::binary), fo(dir / "out.bin", std::ios::binary);
  std::mutex mr, mo;
  auto rec = [](std::ofstream &f, std::mutex &m, const float *x, int n) {
    int64_t t = now_us();
    int32_t k = n;
    std::lock_guard lk(m);
    f.write(reinterpret_cast<const char *>(&t), 8);
    f.write(reinterpret_cast<const char *>(&k), 4);
    f.write(reinterpret_cast<const char *>(x), std::streamsize(n) * 4);
  };
  std::atomic<int64_t> t0{0};
  std::atomic<size_t> pos{0};
  Settings s;
  s.offline = true;
  s.test_talk = [&](float *x, int n) {
    double t = t0 ? (now_us() - t0) / 1e6 : -1;
    if (t < 0 || t > 6) return;
    size_t p = pos;
    for (int i = 0; i < n; ++i) x[i] += talk[(p + size_t(i)) % talk.size()];
    pos = p + size_t(n);
  };
  s.ref_tap = [&](const float *x, int n) { rec(fr, mr, x, n); };
  Engine engine(s);
  engine.start();
  std::unique_ptr<AudioStream> lb;
  auto audio = create_audio_system();
  for (int i = 0; i < 50; ++i) {
    Snapshot sn = engine.snapshot();
    if (sn.mic_dev.running && sn.speaker_dev.running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Snapshot sn = engine.snapshot();
  outf("  talkback output %s, reference: %s\n", sn.speaker_dev.name.c_str(),
       sn.echo_reference.empty() ? "system mix + talkback" : sn.echo_reference[0].c_str());
  lb = audio->open_render_loopback(sn.speaker_dev.id, [&](const float *x, int n) { rec(fo, mo, x, n); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  t0 = now_us();
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));
  std::string inner = "powershell -NoProfile -WindowStyle Hidden -Command (New-Object Media.SoundPlayer '" +
                      wav.string() + "').PlaySync()";
  SpawnOptions o;
  o.background = false;  // a detached launcher cannot reach WMI here
  auto p = spawn({"powershell", "-NoProfile", "-NonInteractive", "-Command",
                  "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=\"" + inner +
                      "\"} | Out-Null"}, o);
  std::this_thread::sleep_for(std::chrono::milliseconds(11000));
  lb.reset();
  engine.stop();
  out("  wrote ref.bin and out.bin\n");
  return 0;
}

}  // namespace

int cmd_echotest(const std::vector<std::string> &args) {
  std::filesystem::path dump;
  for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == "--dump") dump = args[i + 1];
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--timing") {
      auto lock = acquire_instance_lock("aoi-host-instance");
      if (!lock) { out("AOI is running. Stop it first.\n"); return 1; }
      log_init(data_dir() / "echotest.log", false);
      return echo_timing(args[i + 1]);
    }
  if (args.size() == 1 && args[0] == "--verbose") g_verbose = true;
  auto lock = acquire_instance_lock("aoi-host-instance");
  if (!lock) {
    out("AOI is running. Stop it first (aoi stop), then run aoi echotest.\n");
    return 1;
  }
  log_init(data_dir() / "echotest.log", g_verbose);

  // The listener's voice (talkback) and the music, at realistic levels.
  auto talk = st::synth_speech(10, 21, 190);
  normalize(talk, -20.f);
  auto music = st::synth_music(22, 22);
  normalize(music, -16.f);
  std::vector<float> stereo(music.size() * 2);
  for (size_t i = 0; i < music.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = music[i];
  auto wav = data_dir() / "echotest-music.wav";
  if (!write_wav(wav, stereo, 2)) { out("could not write " + wav.string() + "\n"); return 1; }

  std::atomic<int64_t> t0{0};
  std::atomic<size_t> talk_pos{0};
  std::mutex mu;
  Phase ph[] = {{"quiet room", 1, 4}, {"listener talking back", 5.5, 14}, {"music from the speakers", 15.5, 24},
                {"both at once", 25.5, 34}};
  std::vector<float> rec;
  Settings s;
  s.offline = true;
  s.test_talk = [&](float *x, int n) {
    double t = (now_us() - t0) / 1e6;
    bool on = t0 && ((t >= 4 && t < 14) || (t >= 24 && t < 34));
    if (!on) return;
    size_t p = talk_pos;
    for (int i = 0; i < n; ++i) x[i] += talk[(p + size_t(i)) % talk.size()];
    talk_pos = p + size_t(n);
  };
  Engine *eng = nullptr;
  s.mic_tap = [&](const float *raw, const float *proc, int n) {
    if (!t0) return;
    double t = (now_us() - t0) / 1e6, a = 0, b = 0;
    for (int i = 0; i < n; ++i) { a += double(raw[i]) * raw[i]; b += double(proc[i]) * proc[i]; }
    std::lock_guard lk(mu);
    for (auto &p : ph)
      if (t >= p.from && t < p.to) { p.raw += a; p.out += b; p.n += n; }
    if (!dump.empty())
      for (int i = 0; i < n; ++i) { rec.push_back(raw[i]); rec.push_back(proc[i]); }
  };

  Engine engine(s);
  eng = &engine;
  engine.start();
  out("\n  Echo test: plays a voice and music through this PC's speakers for 35 s.\n"
      "  Keep the room quiet and the volume where you normally have it.\n\n");
  // Wait for the microphone and output to run.
  for (int i = 0; i < 50; ++i) {
    Snapshot sn = engine.snapshot();
    if (sn.mic_dev.running && (sn.speaker_dev.running || !sn.talkback)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Snapshot sn = engine.snapshot();
  outf("  microphone  %s (%s)\n  talkback    %s (%s)\n\n", sn.mic_dev.name.c_str(), sn.mic_dev.reason.c_str(),
       sn.speaker_dev.name.c_str(), sn.speaker_dev.reason.c_str());
  if (!sn.mic_dev.running) {
    out("  The microphone is not running" + (sn.mic_dev.error.empty() ? std::string() : ": " + sn.mic_dev.error) + "\n");
    engine.stop();
    return 1;
  }
  t0 = now_us();
  std::unique_ptr<Process> player;
  // The player must not be our child: the system capture leaves out AOI's own
  // process tree, as it does the talkback. WMI starts it on its own.
  auto play = [&] {
    std::string inner = "powershell -NoProfile -WindowStyle Hidden -Command (New-Object Media.SoundPlayer '" +
                        wav.string() + "').PlaySync()";
    std::string cmd = "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=\"" +
                      inner + "\"} | Out-Null";
    SpawnOptions o;
    o.background = false;  // a detached launcher cannot reach WMI here
    std::string err;
    player = spawn({"powershell", "-NoProfile", "-NonInteractive", "-Command", cmd}, o, &err);
    if (!player) LOGW("could not start the music player: %s", err.c_str());
  };
  bool m1 = false;
  int voiced[4] = {0, 0, 0, 0}, blocks[4] = {0, 0, 0, 0};
  while (true) {
    double t = (now_us() - t0) / 1e6;
    if (t >= 35) break;
    if (!m1 && t >= 11.5) { m1 = true; play(); }  // ~2.5 s until it plays; 22 s of music
    Snapshot q = engine.snapshot();
    for (int k = 0; k < 4; ++k)
      if (t >= ph[k].from && t < ph[k].to) { ++blocks[k]; voiced[k] += q.voice.voice; }
    outf("\r  %4.1f s  raw mic %6.1f dBFS   sent %6.1f dBFS   ", t, q.voice.echo_in_dbfs, q.mic_level_db);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Snapshot end = engine.snapshot();
  engine.stop();
  if (player && player->wait(2000)) LOGI("music launcher exit code %d", player->exit_code());
  out("\r                                                                     \r");

  auto db = [](double e, double n) { return n > 0 ? 10 * std::log10(e / n + 1e-20) : -200.0; };
  const Phase &q = ph[0];
  double floor_raw = db(q.raw, q.n);
  outf("  %-26s %10s %10s %10s %8s\n", "", "raw mic", "sent", "removed", "voice?");
  for (int k = 0; k < 4; ++k) {
    const Phase &p = ph[k];
    double r = db(p.raw, p.n), o = db(p.out, p.n);
    outf("  %-26s %7.1f dB %7.1f dB %7.1f dB %7.0f %%\n", p.name, r, o, r - o,
         blocks[k] ? 100.0 * voiced[k] / blocks[k] : 0.0);
  }
  const VoiceStats &v = end.voice;
  outf("\n  echo canceller: %s, delay %s, ERLE %s, echo return loss %s\n", v.aec ? "AEC3" : "off",
       v.delay_ms ? (std::to_string(*v.delay_ms) + " ms").c_str() : "-",
       v.erle_db ? st::f1("%.1f dB", *v.erle_db).c_str() : "-", v.erl_db ? st::f1("%.1f dB", *v.erl_db).c_str() : "-");
  outf("  room level %.1f dBFS. Echo below the room level is inaudible to listeners.\n\n", floor_raw);
  if (!dump.empty()) {
    if (write_wav(dump, rec, 2)) out("  wrote " + dump.string() + " (left: raw mic, right: sent)\n\n");
  }
  (void)eng;
  std::error_code ec;
  std::filesystem::remove(wav, ec);
  return 0;
}

}  // namespace aoi
