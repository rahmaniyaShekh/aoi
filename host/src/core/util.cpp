#include "util.h"

#include <algorithm>
#include <cstdarg>
#include <fstream>

namespace aoi {

std::atomic<bool> g_verbose{false};

// ---- logging -----------------------------------------------------------------
namespace {
std::mutex g_log_mu;
std::ofstream g_log;
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
  g_log.open(file, std::ios::binary | std::ios::app);
}

void log_write(Level lv, const char *fmt, ...) {
  if (lv == Level::Debug && !g_verbose) return;
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  static const char *tag[] = {"DBG", "INF", "WRN", "ERR"};
  std::string line = local_timestamp() + " " + tag[int(lv)] + " " + msg + "\n";
  std::lock_guard lk(g_log_mu);
  if (g_log.is_open()) {
    g_log.write(line.data(), std::streamsize(line.size()));
    g_log.flush();
    // Rotate a long-running log instead of letting it grow without bound.
    if (g_log.tellp() > 8 * 1024 * 1024) {
      g_log.close();
      std::error_code ec;
      auto old = g_log_path; old += ".1";
      std::filesystem::remove(old, ec);
      std::filesystem::rename(g_log_path, old, ec);
      g_log.open(g_log_path, std::ios::binary | std::ios::app);
    }
  }
  if (g_console) { fwrite(line.data(), 1, line.size(), stdout); fflush(stdout); }
  debug_trace(line.c_str());
}

// ---- bytes -----------------------------------------------------------------------
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
  auto tmp = p; tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    if (!f) return false;
  }
  return replace_file(tmp, p);
}

std::string lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return s;
}

}  // namespace aoi
