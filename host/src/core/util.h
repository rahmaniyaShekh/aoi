// Small shared helpers: logging, files, strings, randomness, byte order.
// Clocks, paths and the like come from the platform (platform/os.h).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "platform/os.h"

namespace aoi {

// ---- logging ---------------------------------------------------------------
enum class Level { Debug, Info, Warn, Error };
void log_init(const std::filesystem::path &file, bool console);
void log_write(Level lv, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
#define LOGD(...) ::aoi::log_write(::aoi::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::aoi::log_write(::aoi::Level::Info, __VA_ARGS__)
#define LOGW(...) ::aoi::log_write(::aoi::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::aoi::log_write(::aoi::Level::Error, __VA_ARGS__)
extern std::atomic<bool> g_verbose;

// ---- bytes -----------------------------------------------------------------
std::string random_hex(size_t bytes);
std::string to_hex(const uint8_t *p, size_t n);
uint32_t random_u32();

bool read_file(const std::filesystem::path &p, std::string &out);
bool write_file_atomic(const std::filesystem::path &p, const std::string &data);

std::string lower_ascii(std::string s);

inline void put_be16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put_be24(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 16); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v); }
inline void put_be32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be24(const uint8_t *p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
inline uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Sequence-number arithmetic with wraparound (RFC 1982 style).
inline bool seq_newer(uint16_t a, uint16_t b) { return a != b && uint16_t(a - b) < 0x8000; }
inline bool ts_newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }

// Exponentially weighted moving average helper.
struct Ewma {
  double v = 0; bool init = false;
  void add(double x, double alpha) { v = init ? v + alpha * (x - v) : x; init = true; }
};

}  // namespace aoi
