// aoi - share this PC's audio over the internet. Windows entry point: the
// arguments arrive as UTF-16 and go to the portable command dispatcher.
#include <windows.h>

#include <string>
#include <vector>

#include "core/cli.h"
#include "platform/os.h"

int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(aoi::narrow(argv[i]));
  return aoi::aoi_main(args);
}
