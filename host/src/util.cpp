#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <cstdarg>
#include <ctime>
#include <fstream>

namespace aoi {

std::atomic<bool> g_verbose{false};

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

// ---- logging -----------------------------------------------------------------
namespace {
std::mutex g_log_mu;
FILE *g_log = nullptr;
bool g_console = false;
std::filesystem::path g_log_path;
}  // namespace

void log_init(const std::filesystem::path &file, bool console) {
  std::lock_guard lk(g_log_mu);
  g_console = console;
  g_log_path = file;
  std::error_code ec;
  // Keep one previous log; a long-running host should not fill the disk.
  if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 4 * 1024 * 1024) {
    auto old = file; old += ".1";
    std::filesystem::rename(file, old, ec);
  }
  g_log = _wfopen(file.c_str(), L"ab");
}

void log_write(Level lv, const char *fmt, ...) {
  if (lv == Level::Debug && !g_verbose) return;
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  static const char *tag[] = {"DBG", "INF", "WRN", "ERR"};
  char line[2200];
  int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s %s\n", st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag[int(lv)], msg);
  std::lock_guard lk(g_log_mu);
  if (g_log) {
    fwrite(line, 1, size_t(n), g_log);
    fflush(g_log);
    // Rotate a long-running log instead of letting it grow without bound.
    if (ftell(g_log) > 8 * 1024 * 1024) {
      fclose(g_log);
      std::error_code ec;
      auto old = g_log_path; old += ".1";
      std::filesystem::remove(old, ec);
      std::filesystem::rename(g_log_path, old, ec);
      g_log = _wfopen(g_log_path.c_str(), L"ab");
    }
  }
  if (g_console) { fwrite(line, 1, size_t(n), stdout); fflush(stdout); }
  OutputDebugStringA(line);
}

// ---- paths & strings -----------------------------------------------------------
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

std::string computer_name() {
  wchar_t buf[256];
  DWORD n = DWORD(std::size(buf));
  if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return narrow({buf, n});
  return "Windows PC";
}

std::string hr_text(long hr) {
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

// ---- bytes -----------------------------------------------------------------------
bool random_bytes(void *out, size_t n) {
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string to_hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
  return s;
}

std::string random_hex(size_t bytes) {
  std::vector<uint8_t> b(bytes);
  random_bytes(b.data(), b.size());
  return to_hex(b.data(), b.size());
}

uint32_t random_u32() { uint32_t v; random_bytes(&v, sizeof v); return v; }

bool read_file(const std::filesystem::path &p, std::string &out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool write_file_atomic(const std::filesystem::path &p, const std::string &data) {
  auto tmp = p; tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    if (!f) return false;
  }
  return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace aoi
