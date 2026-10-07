// Console output helpers and the `aoi` subcommands.
#pragma once
#include <string>
#include <vector>

namespace aoi {

void con_init();
void out(const std::string &utf8);
void outf(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void print_qr(const std::string &text);

// ANSI styles (empty when the console cannot render them).
extern const char *C_DIM, *C_BOLD, *C_ACC, *C_OK, *C_WARN, *C_BAD, *C_RST;

int cmd_start(const std::vector<std::wstring> &args);
int cmd_run(const std::vector<std::wstring> &args);
int cmd_daemon(const std::vector<std::wstring> &args);
int cmd_stop();
int cmd_status(bool watch, bool as_json);
int cmd_code();
int cmd_logs(bool follow);
int cmd_new_code();
int cmd_kick(int id);
int cmd_install(bool start_after);
int cmd_uninstall(bool purge);
int cmd_update();
int cmd_help();
int run_selftest(const std::vector<std::wstring> &args);

}  // namespace aoi
