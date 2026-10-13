// Operating-system services the core needs: clocks, paths, randomness,
// processes, the console. Interface only; implemented in platform/win.
// Core code reaches the OS through this file,
// audio_system.h, http.h, ipc.h and installer.h, and nothing else.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aoi {

// ---- time ------------------------------------------------------------------
int64_t now_us();             // monotonic, high resolution
inline double now_s() { return now_us() * 1e-6; }
uint64_t ntp_now();           // wall clock as a 64-bit NTP timestamp (RTCP sender reports)
std::string local_timestamp(); // "2026-10-07 14:02:03.123", for the log
// Ask for ~1 ms scheduler resolution while a timing-critical thread runs
// (the network simulator). Calls nest.
void fine_timer(bool on);
// The calling thread runs ahead of ordinary work (the per-listener encoder).
void raise_thread_priority();

// ---- text, paths, identity -------------------------------------------------
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::filesystem::path data_dir();  // per-user state (%LOCALAPPDATA%\AOI), created on demand
std::filesystem::path exe_path();
std::string computer_name();
std::string os_error_text(long code);  // "0x80070005 (Access is denied.)"
std::string platform_name();           // "Windows 11 (26300)" for the user agent and logs
// Atomic replace: write `from`, then move it over `to`.
bool replace_file(const std::filesystem::path &from, const std::filesystem::path &to);
void debug_trace(const char *line);    // debugger output (no-op where there is none)

// ---- randomness ------------------------------------------------------------
bool random_bytes(void *out, size_t n);  // OS CSPRNG

// ---- processes ---------------------------------------------------------------
int current_pid();

class Process {
 public:
  virtual ~Process() = default;
  virtual bool wait(int timeout_ms) = 0;  // true once exited
  virtual int exit_code() = 0;
  virtual void terminate() = 0;
  virtual int pid() const = 0;
};

struct SpawnOptions {
  bool background = false;  // detached, no console window (the daemon)
  std::filesystem::path cwd;
};
// argv[0] is the program (absolute, or found on PATH).
std::unique_ptr<Process> spawn(const std::vector<std::string> &argv, const SpawnOptions &opt, std::string *err = nullptr);
std::unique_ptr<Process> open_process(int pid);  // null if not running

// One host per user. Null when another process holds it.
class InstanceLock {
 public:
  virtual ~InstanceLock() = default;
};
std::unique_ptr<InstanceLock> acquire_instance_lock(const std::string &name);

// Ctrl+C, console close, logoff: `fn` runs once on another thread.
void on_interrupt(std::function<void()> fn);
void detach_console();  // the daemon: no console window of its own
[[noreturn]] void hard_exit(int code);

// ---- console -----------------------------------------------------------------
bool console_init();  // UTF-8 output; true when ANSI styles render
void console_write(const std::string &utf8);

}  // namespace aoi
