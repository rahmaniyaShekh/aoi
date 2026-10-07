// platform/os.h on Windows.
#include "platform/os.h"

#include <windows.h>
#include <bcrypt.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace aoi {

// ---- time ------------------------------------------------------------------
int64_t now_us() {
  static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t((__int128)c.QuadPart * 1000000 / freq);
}

uint64_t ntp_now() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
  // FILETIME counts from 1601, NTP from 1900: 9435484800 s apart.
  uint64_t us = t / 10;
  uint64_t secs = us / 1000000 - 9435484800ull;
  uint64_t frac = ((us % 1000000) << 32) / 1000000;
  return (secs << 32) | frac;
}

std::string local_timestamp() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[40];
  snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond, st.wMilliseconds);
  return b;
}

void raise_thread_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }

void fine_timer(bool on) {
  if (on) timeBeginPeriod(1);
  else timeEndPeriod(1);
}

// ---- text, paths, identity -------------------------------------------------
std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string o(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), o.data(), n, nullptr, nullptr);
  return o;
}

std::filesystem::path data_dir() {
  static std::filesystem::path dir = [] {
    PWSTR p = nullptr;
    std::filesystem::path d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = p;
    CoTaskMemFree(p);
    if (d.empty()) d = std::filesystem::temp_directory_path();
    d /= L"AOI";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
  }();
  return dir;
}

std::filesystem::path exe_path() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return std::filesystem::path(std::wstring(buf, n));
}

std::string computer_name() {
  wchar_t buf[256];
  DWORD n = DWORD(std::size(buf));
  if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return narrow({buf, n});
  return "Windows PC";
}

std::string os_error_text(long hr) {
  char code[16];
  snprintf(code, sizeof code, "0x%08lX", (unsigned long)hr);
  wchar_t *msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, DWORD(hr), 0, (LPWSTR)&msg, 0, nullptr);
  std::string out = code;
  if (msg) {
    std::string t = narrow(msg);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    out += " (" + t + ")";
    LocalFree(msg);
  }
  return out;
}

std::string platform_name() {
  // RtlGetVersion tells the truth regardless of the manifest.
  using Fn = LONG(WINAPI *)(OSVERSIONINFOW *);
  OSVERSIONINFOW v{sizeof v};
  if (auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
  char b[64];
  snprintf(b, sizeof b, "Windows %s (%lu)", v.dwBuildNumber >= 22000 ? "11" : "10", v.dwBuildNumber);
  return b;
}

bool replace_file(const std::filesystem::path &from, const std::filesystem::path &to) {
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void debug_trace(const char *line) { OutputDebugStringA(line); }

// ---- randomness ------------------------------------------------------------
bool random_bytes(void *out, size_t n) {
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// ---- processes ---------------------------------------------------------------
int current_pid() { return int(GetCurrentProcessId()); }

namespace {
class WinProcess final : public Process {
 public:
  WinProcess(HANDLE h, DWORD pid) : h_(h), pid_(pid) {}
  ~WinProcess() override { CloseHandle(h_); }
  bool wait(int ms) override { return WaitForSingleObject(h_, ms < 0 ? INFINITE : DWORD(ms)) == WAIT_OBJECT_0; }
  int exit_code() override { DWORD c = 1; GetExitCodeProcess(h_, &c); return int(c); }
  void terminate() override { TerminateProcess(h_, 1); }
  int pid() const override { return int(pid_); }

 private:
  HANDLE h_;
  DWORD pid_;
};

// CommandLineToArgvW's rules, in reverse.
std::wstring quote_arg(const std::wstring &a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring o = L"\"";
  size_t bs = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { ++bs; continue; }
    if (c == L'"') o.append(bs * 2 + 1, L'\\');
    else o.append(bs, L'\\');
    bs = 0;
    o += c;
  }
  o.append(bs * 2, L'\\');
  return o + L"\"";
}
}  // namespace

std::unique_ptr<Process> spawn(const std::vector<std::string> &argv, const SpawnOptions &opt, std::string *err) {
  std::wstring cmd;
  for (auto &a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote_arg(widen(a)); }
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  DWORD flags = opt.background ? DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW : 0;
  std::wstring cwd = opt.cwd.wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, opt.background ? FALSE : TRUE, flags, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    if (err) *err = os_error_text(long(HRESULT_FROM_WIN32(GetLastError())));
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return std::make_unique<WinProcess>(pi.hProcess, pi.dwProcessId);
}

std::unique_ptr<Process> open_process(int pid) {
  if (pid <= 0) return nullptr;
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!h) return nullptr;
  DWORD code = 0;
  if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) { CloseHandle(h); return nullptr; }
  return std::make_unique<WinProcess>(h, DWORD(pid));
}

namespace {
class WinInstanceLock final : public InstanceLock {
 public:
  explicit WinInstanceLock(HANDLE h) : h_(h) {}
  ~WinInstanceLock() override { CloseHandle(h_); }

 private:
  HANDLE h_;
};

std::function<void()> g_interrupt;
std::once_flag g_interrupt_once;
BOOL WINAPI on_ctrl(DWORD) {
  std::call_once(g_interrupt_once, [] { if (g_interrupt) std::thread(g_interrupt).detach(); });
  return TRUE;
}
}  // namespace

std::unique_ptr<InstanceLock> acquire_instance_lock(const std::string &name) {
  HANDLE m = CreateMutexW(nullptr, TRUE, widen("Local\\" + name).c_str());
  if (!m) return nullptr;
  if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); return nullptr; }
  return std::make_unique<WinInstanceLock>(m);
}

void on_interrupt(std::function<void()> fn) {
  g_interrupt = std::move(fn);
  SetConsoleCtrlHandler(on_ctrl, TRUE);
}

// Started from a terminal a console exe gets a console window: drop it.
void detach_console() { FreeConsole(); }

void hard_exit(int code) { ExitProcess(UINT(code)); }

// ---- console -----------------------------------------------------------------
namespace {
bool g_console_out = false;
}

bool console_init() {
  SetConsoleOutputCP(CP_UTF8);
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  g_console_out = GetConsoleMode(h, &mode) != 0;
  return g_console_out && SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void console_write(const std::string &s) {
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

}  // namespace aoi
