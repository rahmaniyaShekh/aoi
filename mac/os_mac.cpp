// platform/os.h on macOS (POSIX). This part of the port is real.
#include "platform/os.h"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern char **environ;

namespace aoi {

int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

uint64_t ntp_now() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  uint64_t secs = uint64_t(tv.tv_sec) + 2208988800ull;  // 1970 -> 1900
  uint64_t frac = (uint64_t(tv.tv_usec) << 32) / 1000000;
  return (secs << 32) | frac;
}

std::string local_timestamp() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  tm t;
  time_t s = tv.tv_sec;
  localtime_r(&s, &t);
  char b[40];
  snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
           t.tm_min, t.tm_sec, int(tv.tv_usec / 1000));
  return b;
}

void fine_timer(bool) {}  // the macOS scheduler is already fine-grained
void raise_thread_priority() {}  // a real port: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)

// UTF-8 everywhere on macOS; wide strings are only used in paths, as UTF-32.
std::wstring widen(std::string_view s) {
  std::wstring w;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) cp &= (0x3F >> (n - 1));
    for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
    w.push_back(wchar_t(cp));
    i += size_t(n);
  }
  return w;
}

std::string narrow(std::wstring_view w) {
  std::string s;
  for (wchar_t wc : w) {
    uint32_t c = uint32_t(wc);
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
  }
  return s;
}

std::filesystem::path data_dir() {
  static std::filesystem::path dir = [] {
    const char *home = getenv("HOME");
    std::filesystem::path d = home ? std::filesystem::path(home) / "Library" / "Application Support" / "AOI"
                                   : std::filesystem::temp_directory_path() / "AOI";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
  }();
  return dir;
}

std::filesystem::path exe_path() {
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return {};
  std::error_code ec;
  auto p = std::filesystem::canonical(buf, ec);
  return ec ? std::filesystem::path(buf) : p;
}

std::string computer_name() {
  char b[256] = {};
  if (gethostname(b, sizeof b - 1) == 0) return b;
  return "Mac";
}

std::string os_error_text(long code) { return std::to_string(code) + " (" + strerror(int(code)) + ")"; }
std::string platform_name() { return "macOS"; }

bool replace_file(const std::filesystem::path &from, const std::filesystem::path &to) {
  return rename(from.c_str(), to.c_str()) == 0;
}

void debug_trace(const char *) {}

bool random_bytes(void *out, size_t n) {
  arc4random_buf(out, n);
  return true;
}

int current_pid() { return int(getpid()); }

namespace {
class PosixProcess final : public Process {
 public:
  explicit PosixProcess(pid_t p, bool child) : pid_(p), child_(child) {}
  bool wait(int ms) override {
    int64_t until = now_us() + int64_t(ms) * 1000;
    do {
      if (child_) {
        int st = 0;
        pid_t r = waitpid(pid_, &st, WNOHANG);
        if (r == pid_) { code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 1; return true; }
      } else if (kill(pid_, 0) != 0 && errno == ESRCH) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (ms < 0 || now_us() < until);
    return false;
  }
  int exit_code() override { return code_; }
  void terminate() override { kill(pid_, SIGKILL); }
  int pid() const override { return int(pid_); }

 private:
  pid_t pid_;
  bool child_;
  int code_ = 1;
};

class FileLock final : public InstanceLock {
 public:
  explicit FileLock(int fd) : fd_(fd) {}
  ~FileLock() override { close(fd_); }

 private:
  int fd_;
};

std::function<void()> g_interrupt;
void on_signal(int) {
  static std::once_flag once;
  std::call_once(once, [] { if (g_interrupt) std::thread(g_interrupt).detach(); });
}
}  // namespace

std::unique_ptr<Process> spawn(const std::vector<std::string> &argv, const SpawnOptions &opt, std::string *err) {
  std::vector<char *> a;
  for (auto &s : argv) a.push_back(const_cast<char *>(s.c_str()));
  a.push_back(nullptr);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  if (opt.background) posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, a[0], nullptr, &attr, a.data(), environ);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    if (err) *err = strerror(rc);
    return nullptr;
  }
  return std::make_unique<PosixProcess>(pid, true);
}

std::unique_ptr<Process> open_process(int pid) {
  if (pid <= 0 || kill(pid_t(pid), 0) != 0) return nullptr;
  return std::make_unique<PosixProcess>(pid_t(pid), false);
}

std::unique_ptr<InstanceLock> acquire_instance_lock(const std::string &name) {
  int fd = open((data_dir() / (name + ".lock")).c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) return nullptr;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return nullptr; }
  return std::make_unique<FileLock>(fd);
}

void on_interrupt(std::function<void()> fn) {
  g_interrupt = std::move(fn);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
}

void detach_console() {}
void hard_exit(int code) { _exit(code); }

bool console_init() { return isatty(STDOUT_FILENO) != 0; }
void console_write(const std::string &s) {
  fwrite(s.data(), 1, s.size(), stdout);
  fflush(stdout);
}

}  // namespace aoi
