// aoi - share this PC's audio over the internet.
//
//   aoi start        start sharing in the background (prints code, link, QR)
//   aoi status       who is listening, and how well
//   aoi stop         stop sharing
//   aoi help         everything else
#include <windows.h>

#include <string>
#include <vector>

#include "cli.h"
#include "util.h"

using namespace aoi;

int wmain(int argc, wchar_t **argv) {
  con_init();
  std::vector<std::wstring> args(argv + 1, argv + argc);
  std::wstring cmd = args.empty() ? L"start" : args[0];
  std::vector<std::wstring> rest(args.empty() ? args.begin() : args.begin() + 1, args.end());
  // `aoi --simulate ...` is `aoi start --simulate ...`
  if (!cmd.empty() && cmd[0] == L'-' && cmd != L"-h" && cmd != L"--help" && cmd != L"--version") {
    rest = args;
    cmd = L"start";
  }
  auto flag = [&](const wchar_t *f) {
    for (auto &a : rest) if (a == f) return true;
    return false;
  };
  auto arg = [&](size_t i) { return i < rest.size() ? narrow(rest[i]) : std::string(); };

  if (cmd == L"start") return cmd_start(rest);
  if (cmd == L"run") return cmd_run(rest);
  if (cmd == L"daemon") return cmd_daemon(rest);
  if (cmd == L"stop" || cmd == L"kill") return cmd_stop();
  if (cmd == L"restart") { cmd_stop(); return cmd_start(rest); }
  if (cmd == L"status") return cmd_status(flag(L"--watch") || flag(L"-w"), flag(L"--json"));
  if (cmd == L"code" || cmd == L"link" || cmd == L"qr") return cmd_code();
  if (cmd == L"logs" || cmd == L"log") return cmd_logs(flag(L"-f") || flag(L"--follow"));
  if (cmd == L"new-code") return cmd_new_code();
  if (cmd == L"kick") return cmd_kick(atoi(arg(0).c_str()));
  if (cmd == L"install") return cmd_install(!flag(L"--no-start"));
  if (cmd == L"uninstall") return cmd_uninstall(flag(L"--purge"));
  if (cmd == L"selftest") return run_selftest(rest);
  if (cmd == L"version" || cmd == L"--version") { outf("aoi %s\n", AOI_VERSION); return 0; }
  if (cmd == L"help" || cmd == L"-h" || cmd == L"--help") return cmd_help();
  outf("unknown command '%s'. Try: aoi help\n", narrow(cmd).c_str());
  return 2;
}
