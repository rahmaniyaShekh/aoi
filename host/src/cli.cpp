#include "cli.h"

#include <windows.h>
#include <objbase.h>

#include <qrcodegen.hpp>
#include <nlohmann/json.hpp>

#include <cstdarg>
#include <fstream>

#include "control.h"
#include "engine.h"
#include "install.h"
#include "util.h"

using json = nlohmann::json;

namespace aoi {

const char *C_DIM = "\x1b[2m", *C_BOLD = "\x1b[1m", *C_ACC = "\x1b[38;5;141m", *C_OK = "\x1b[32m",
           *C_WARN = "\x1b[33m", *C_BAD = "\x1b[31m", *C_RST = "\x1b[0m";
static bool g_console_out = false;

void con_init() {
  SetConsoleOutputCP(CP_UTF8);
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  g_console_out = GetConsoleMode(h, &mode) != 0;
  if (!g_console_out || !SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    C_DIM = C_BOLD = C_ACC = C_OK = C_WARN = C_BAD = C_RST = "";
}

void out(const std::string &s) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (g_console_out) {
    std::wstring w = widen(s);
    DWORD n;
    WriteConsoleW(h, w.data(), DWORD(w.size()), &n, nullptr);
  } else {
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
  }
}

void outf(const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  out(buf);
}

// Two QR rows per text line with half blocks, explicit white-on-black so it
// scans the same on a light or a dark terminal.
void print_qr(const std::string &text) {
  using qrcodegen::QrCode;
  QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::LOW);
  int n = qr.getSize(), b = 2;
  bool color = C_RST[0] != 0;
  auto light = [&](int x, int y) { return x < 0 || y < 0 || x >= n || y >= n || !qr.getModule(x, y); };
  std::string s;
  for (int y = -b; y < n + b; y += 2) {
    s += "    ";
    if (color) s += "\x1b[97;40m";
    for (int x = -b; x < n + b; ++x) {
      bool t = light(x, y), u = light(x, y + 1);
      s += t && u ? "█" : t ? "▀" : u ? "▄" : " ";
    }
    if (color) s += "\x1b[0m";
    s += "\n";
  }
  out(s);
}

// ---------------------------------------------------------------------------------------
namespace {

std::optional<json> status_now(int timeout = 1500) {
  auto r = control_call({{"cmd", "status"}}, timeout);
  if (!r || !r->value("ok", false)) return std::nullopt;
  return (*r)["status"];
}

std::string fmt_dur(double s) {
  char b[32];
  int t = int(s);
  if (t >= 3600) snprintf(b, sizeof b, "%dh%02dm", t / 3600, t / 60 % 60);
  else if (t >= 60) snprintf(b, sizeof b, "%dm%02ds", t / 60, t % 60);
  else snprintf(b, sizeof b, "%ds", t);
  return b;
}

void print_banner(const json &st) {
  outf("\n  %s%s♪ AOI is sharing this PC's audio%s\n\n", C_BOLD, C_ACC, C_RST);
  outf("    %sCode%s   %s%s%s\n", C_DIM, C_RST, C_BOLD, st.value("pretty", "").c_str(), C_RST);
  outf("    %sLink%s   %s\n\n", C_DIM, C_RST, st.value("link", "").c_str());
  print_qr(st.value("link", ""));
  outf("\n    Open the link on any phone or laptop, or enter the code at\n    %s\n\n",
       st.value("link", "").substr(0, st.value("link", "").rfind('/')).c_str());
  outf("    %saoi status%s  who is listening    %saoi stop%s  stop sharing    %saoi help%s  more\n\n", C_BOLD, C_RST,
       C_BOLD, C_RST, C_BOLD, C_RST);
}

void print_status(const json &st) {
  bool pub = st.value("published", false);
  int n = int(st["listeners"].size());
  const char *dot = pub ? C_OK : C_WARN;
  outf("\n  %s●%s %sAOI%s  %s   %scode%s %s%s%s   %s\n", dot, C_RST, C_BOLD, C_RST, st.value("status", "").c_str(), C_DIM,
       C_RST, C_BOLD, st.value("pretty", "").c_str(), C_RST, st.value("link", "").c_str());
  if (!st.value("error", "").empty()) outf("    %s%s%s\n", C_WARN, st.value("error", "").c_str(), C_RST);
  outf("\n    %-10s %s", "Sending", st.value("source", "").c_str());
  if (st.value("auto_level", false)) outf("  %s(level %+.1f dB)%s", C_DIM, st.value("gain_db", 0.0), C_RST);
  outf("\n");
  if (st.value("mic", false)) {
    std::string mn = st.value("mic_name", "");
    outf("    %-10s %s%s\n", "Mic", mn.empty() ? "no microphone found" : mn.c_str(),
         st.value("mic_active", false) ? "  • speaking" : "");
  } else {
    outf("    %-10s off\n", "Mic");
  }
  outf("    %-10s %s\n", "Talkback", st.value("talkback", false) ? ("on → " + st.value("talk_device", "")).c_str() : "off");
  outf("    %-10s up to %d kbps, adapts per listener%s\n", "Quality", st.value("max_kbps", 256),
       st.value("turn", false) ? " · relay available" : "");
  int live_n = 0;
  for (auto &l : st["listeners"]) live_n += l.value("state", "") == "live";
  outf("\n    %sListeners %d/%d%s\n", C_BOLD, live_n, st.value("max_listeners", 2), C_RST);
  double lf = st.value("last_failed_s", -1.0);
  if (lf >= 0 && lf < 600)
    outf("    %s%d join attempt(s) could not connect directly (last %.0f s ago); their page switches to the relay%s\n",
         C_DIM, st.value("failed_connects", 0), lf, C_RST);
  if (!n) outf("    %snobody yet - share the link%s\n", C_DIM, C_RST);
  for (auto &l : st["listeners"]) {
    std::string name = l.value("name", "");
    if (name.empty()) name = "Listener " + std::to_string(l.value("id", 0));
    std::string state = l.value("state", "");
    int q = l.value("quality", 0);
    std::string bars;
    static const char *glyph[4] = {"▂", "▄", "▆", "█"};
    for (int i = 0; i < 4; ++i) bars += i < q ? glyph[i] : "·";
    const char *qc = q >= 3 ? C_OK : q == 2 ? C_WARN : C_BAD;
    outf("    #%-2d %-18s %s%s%s  ", l.value("id", 0), name.c_str(), qc, bars.c_str(), C_RST);
    if (state != "live") { outf("%s%s…%s\n", C_DIM, state.c_str(), C_RST); continue; }
    outf("%s  %.0f kbps %s", fmt_dur(l.value("connected_s", 0.0)).c_str(), l.value("opus_kbps", 0.0),
         l.value("stereo", true) ? "stereo" : "mono");
    outf("  · %.0f ms  · loss %.1f%%", l.value("rtt_ms", 0.0), l.value("loss", 0.0) * 100);
    std::string path = l.value("path", "");
    outf("  · %s", path.empty() ? "direct" : path.c_str());
    if (l.value("buffer_ms", 0.0) > 0) outf("  · buffer %.0f ms", l.value("buffer_ms", 0.0));
    if (l.value("red", 0) > 0) outf("  · %sprotected x%d%s", C_ACC, l.value("red", 0), C_RST);
    if (l.value("talking", false)) outf("  · %stalking%s", C_OK, C_RST);
    outf("\n");
    outf("        %smix: music %+.0f dB%s, voice %+.0f dB%s  ·  %s  ·  %s%s\n", C_DIM, l.value("music_db", 0.0),
         l.value("music_mute", false) ? " (muted)" : "", l.value("voice_db", 0.0), l.value("voice_mute", false) ? " (muted)" : "",
         l.value("cc", "").c_str(), l.value("why", "").c_str(), C_RST);
  }
  outf("\n");
}

std::vector<std::wstring> passthrough(const std::vector<std::wstring> &args) {
  std::vector<std::wstring> o;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::wstring &a = args[i];
    if ((a == L"--simulate" || a == L"--service") && i + 1 < args.size()) { o.push_back(a); o.push_back(args[++i]); }
    else if (a == L"--new-code" || a == L"--verbose") o.push_back(a);
  }
  return o;
}

void apply_flags(Settings &s, const std::vector<std::wstring> &args) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::wstring &a = args[i];
    if (a == L"--simulate" && i + 1 < args.size()) {
      if (!parse_netsim(narrow(args[++i]), s.sim)) LOGW("bad --simulate spec");
    } else if (a == L"--service" && i + 1 < args.size()) {
      s.service = narrow(args[++i]);
    } else if (a == L"--new-code") {
      s.new_code = true;
    } else if (a == L"--verbose") {
      g_verbose = true;
    }
  }
}

HANDLE g_stop_event = nullptr;
BOOL WINAPI on_ctrl(DWORD) {
  if (g_stop_event) SetEvent(g_stop_event);
  return TRUE;
}

// The host itself, in this process: detached (daemon) or in the terminal (run).
int serve(const std::vector<std::wstring> &args, bool foreground) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\aoi-host-instance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (foreground) out("AOI is already running in the background. Use: aoi status / aoi stop\n");
    return 0;
  }
  log_init(data_dir() / L"aoi.log", foreground);
  Settings s;  // fixed best-outcome defaults; nothing to configure
  apply_flags(s, args);
  LOGI("aoi %s starting (pid %lu)%s", AOI_VERSION, GetCurrentProcessId(), foreground ? " in foreground" : "");
  if (s.sim.active()) LOGW("network simulator active: %s", s.sim.describe().c_str());
  write_file_atomic(data_dir() / L"aoi.pid", std::to_string(GetCurrentProcessId()));
  cleanup_old_copies();  // leftovers from an update, unlocked now
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  SetConsoleCtrlHandler(on_ctrl, TRUE);

  Engine engine(s);
  engine.start();
  ControlServer ctl(engine, [] { SetEvent(g_stop_event); });
  ctl.start();

  if (foreground) {
    bool shown = false;
    while (WaitForSingleObject(g_stop_event, 250) == WAIT_TIMEOUT) {
      if (!shown && engine.snapshot().published) {
        shown = true;
        print_banner(snapshot_json(engine.snapshot()));
        out("    Press Ctrl+C to stop.\n\n");
      }
    }
  } else {
    WaitForSingleObject(g_stop_event, INFINITE);
  }
  LOGI("stopping");
  // A clean stop takes about a second (listeners told, code withdrawn). If
  // anything below ever wedges, still exit promptly: `aoi stop` must stop.
  std::thread([] {
    Sleep(4000);
    LOGW("shutdown took too long; exiting");
    ExitProcess(0);
  }).detach();
  ctl.stop();
  engine.stop();
  std::error_code ec;
  std::filesystem::remove(data_dir() / L"aoi.pid", ec);
  LOGI("stopped");
  CloseHandle(mutex);
  return 0;
}

std::vector<std::string> tail_lines(const std::filesystem::path &p, size_t n) {
  std::ifstream f(p, std::ios::binary);
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(f, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
    if (lines.size() > n) lines.erase(lines.begin());
  }
  return lines;
}

DWORD daemon_pid() {
  std::string s;
  if (!read_file(data_dir() / L"aoi.pid", s)) return 0;
  return DWORD(strtoul(s.c_str(), nullptr, 10));
}

}  // namespace

// ---------------------------------------------------------------------------------------
int cmd_daemon(const std::vector<std::wstring> &args) {
  // Started from the Run key a console exe gets a console window: drop it.
  FreeConsole();
  return serve(args, false);
}

int cmd_run(const std::vector<std::wstring> &args) { return serve(args, true); }

int cmd_start(const std::vector<std::wstring> &args) {
  if (auto st = status_now()) {
    if (st->value("published", false)) {
      print_banner(*st);
    } else {
      outf("AOI is already running (%s).\n", st->value("status", "").c_str());
    }
    return 0;
  }
  std::wstring cmdline = L"\"" + exe_path().wstring() + L"\" daemon";
  for (auto &a : passthrough(args)) cmdline += L" \"" + a + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, data_dir().c_str(), &si, &pi)) {
    outf("%scould not start the background process: %s%s\n", C_BAD, hr_text(long(HRESULT_FROM_WIN32(GetLastError()))).c_str(), C_RST);
    return 1;
  }
  CloseHandle(pi.hThread);
  out("  Starting AOI");
  int64_t until = now_us() + 40000000;
  std::string last_status;
  while (now_us() < until) {
    if (WaitForSingleObject(pi.hProcess, 300) == WAIT_OBJECT_0) {
      CloseHandle(pi.hProcess);
      if (auto st = status_now()) { print_banner(*st); return 0; }  // another instance won the race
      outf("\n%sAOI exited during startup. Last log lines:%s\n", C_BAD, C_RST);
      for (auto &l : tail_lines(data_dir() / L"aoi.log", 12)) outf("    %s\n", l.c_str());
      return 1;
    }
    auto st = status_now(800);
    if (st && st->value("published", false)) {
      CloseHandle(pi.hProcess);
      out("\r              \r");
      print_banner(*st);
      return 0;
    }
    if (st && st->value("status", "") != last_status) {
      last_status = st->value("status", "");
    }
    out(".");
  }
  CloseHandle(pi.hProcess);
  outf("\n  %sStill connecting (%s). It keeps trying in the background; check with: aoi status%s\n", C_WARN,
       last_status.c_str(), C_RST);
  return 0;
}

int cmd_stop() {
  auto st = status_now();
  DWORD pid = st ? DWORD(st->value("pid", 0)) : daemon_pid();
  HANDLE proc = pid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;
  if (!st && !proc) { out("AOI is not running.\n"); return 0; }
  if (st) control_call({{"cmd", "stop"}}, 2000);
  if (proc) {
    // Graceful first (listeners are told, the code is withdrawn), then force.
    if (WaitForSingleObject(proc, 6000) != WAIT_OBJECT_0) {
      TerminateProcess(proc, 1);
      WaitForSingleObject(proc, 2000);
      out("AOI stopped (forced).\n");
    } else {
      out("AOI stopped.\n");
    }
    CloseHandle(proc);
  } else {
    out("AOI stopped.\n");
  }
  std::error_code ec;
  std::filesystem::remove(data_dir() / L"aoi.pid", ec);
  return 0;
}

int cmd_status(bool watch, bool as_json) {
  do {
    auto st = status_now();
    if (!st) {
      out("AOI is not running. Start it with: aoi start\n");
      return 1;
    }
    if (as_json) { out(st->dump(2) + "\n"); return 0; }
    if (watch) out("\x1b[H\x1b[2J");
    print_status(*st);
    if (watch) Sleep(1000);
  } while (watch);
  return 0;
}

int cmd_code() {
  auto st = status_now();
  if (!st) {
    out("AOI is not running. Start it with: aoi start\n");
    return 1;
  }
  print_banner(*st);
  return 0;
}

int cmd_logs(bool follow) {
  auto path = data_dir() / L"aoi.log";
  for (auto &l : tail_lines(path, 60)) outf("%s\n", l.c_str());
  if (!follow) return 0;
  std::error_code ec;
  uintmax_t pos = std::filesystem::file_size(path, ec);
  for (;;) {
    Sleep(500);
    uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec) continue;
    if (sz < pos) pos = 0;  // rotated
    if (sz > pos) {
      std::ifstream f(path, std::ios::binary);
      f.seekg(std::streamoff(pos));
      std::string chunk(size_t(sz - pos), '\0');
      f.read(chunk.data(), std::streamsize(chunk.size()));
      out(chunk);
      pos = sz;
    }
  }
}

int cmd_new_code() {
  if (control_call({{"cmd", "new-code"}})) {
    out("New code requested. Everyone on the old code is disconnected.\n");
    for (int i = 0; i < 40; ++i) {
      Sleep(250);
      auto st = status_now();
      if (st && st->value("published", false)) { print_banner(*st); return 0; }
    }
    return 0;
  }
  std::error_code ec;
  std::filesystem::remove(data_dir() / L"host.json", ec);
  out("AOI is not running; a new code will be made when it starts.\n");
  return 0;
}

int cmd_kick(int id) {
  if (!control_call({{"cmd", "kick"}, {"id", id}})) { out("AOI is not running.\n"); return 1; }
  outf("Listener #%d removed.\n", id);
  return 0;
}

int cmd_install(bool start_after) {
  std::string err;
  bool was_running = status_now().has_value();
  if (was_running && !is_installed_copy()) cmd_stop();
  if (!install(&err)) { outf("%sInstall failed: %s%s\n", C_BAD, err.c_str(), C_RST); return 1; }
  outf("\n  %sAOI installed%s to %s\n", C_OK, C_RST, narrow(install_dir().wstring()).c_str());
  out("  `aoi` now works in any new terminal: aoi start / aoi status / aoi stop\n");
  if (start_after) {
    std::wstring cmd = L"\"" + installed_exe().wstring() + L"\" start";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 60000);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
  }
  return 0;
}

// Fetches the latest release with the GitHub CLI (the repository is private)
// and installs it over this one; a running host is restarted on the new build.
int cmd_update(const std::wstring &from) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::filesystem::path dir = std::filesystem::path(tmp) / (L"aoi-update-" + widen(random_hex(4)));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto exe = dir / L"aoi.exe";
  STARTUPINFOW si{sizeof si};
  DWORD code = 1;
  if (!from.empty()) {
    // `aoi update --from <file>`: install a local build exactly the way a
    // downloaded release is installed (offline machines, and testing).
    if (!CopyFileW(from.c_str(), exe.c_str(), FALSE)) { outf("%s  Cannot read that file%s\n", C_BAD, C_RST); return 1; }
  } else {
    outf("  Downloading the latest AOI release...\n");
    std::wstring cmd = L"gh release download -R rahmaniyaShekh/aoi -p aoi.exe --clobber -D \"" + dir.wstring() + L"\"";
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 300000);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } else {
      outf("%s  The GitHub CLI is needed: winget install --id GitHub.cli, then gh auth login%s\n", C_BAD, C_RST);
      return 1;
    }
    if (code != 0 || !std::filesystem::exists(exe, ec)) {
      outf("%s  Download failed (logged in with gh, with access to the repository?)%s\n", C_BAD, C_RST);
      return 1;
    }
  }
  // The new exe installs itself (stopping this one if it runs) and starts.
  std::wstring inst = L"\"" + exe.wstring() + L"\" install";
  PROCESS_INFORMATION p2{};
  if (!CreateProcessW(nullptr, inst.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &p2)) return 1;
  WaitForSingleObject(p2.hProcess, 120000);
  GetExitCodeProcess(p2.hProcess, &code);
  CloseHandle(p2.hThread);
  CloseHandle(p2.hProcess);
  std::filesystem::remove_all(dir, ec);
  return int(code);
}

int cmd_uninstall(bool purge) {
  cmd_stop();
  std::string err;
  uninstall(purge, &err);
  outf("AOI uninstalled%s.%s\n", purge ? " and its settings removed" : "", err.empty() ? "" : (" (" + err + ")").c_str());
  return 0;
}

int cmd_help() {
  outf("\n  %sAOI%s %s - share this PC's audio over the internet\n\n", C_BOLD, C_RST, AOI_VERSION);
  out("    aoi start             start sharing in the background; prints code, link and QR\n"
      "    aoi status [-w]       who is listening and how well (-w: live)\n"
      "    aoi code              show the code, link and QR again\n"
      "    aoi stop              stop sharing (also: taskkill /im aoi.exe)\n"
      "    aoi restart           stop and start\n"
      "    aoi logs [-f]         show (follow) the log\n"
      "    aoi new-code          new code; disconnects everyone on the old one\n"
      "    aoi kick <n>          remove listener #n\n"
      "\n"
      "    aoi update            install the latest release (uses the GitHub CLI)\n"
      "    aoi install           install for this user (PATH + Apps & features, no admin)\n"
      "    aoi uninstall [--purge]\n"
      "    aoi run               run in this terminal instead of the background\n"
      "    aoi selftest          check codecs, networking maths and crypto on this PC\n"
      "\n"
      "    Testing a bad network:  aoi start --simulate rate=48,loss=5,burst=3,delay=150,jitter=40\n\n"
      "    Always on, nothing to configure: system audio + your microphone (if any),\n"
      "    talkback, auto level, adaptive Opus up to 256 kbps stereo with loss protection.\n"
      "    Each listener sets their own music/voice balance on their page.\n"
      "    Listeners open the link in a browser - no app, no account. Up to 2 at a time.\n\n");
  return 0;
}

}  // namespace aoi
