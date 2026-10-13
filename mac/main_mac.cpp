// aoi on macOS: arguments are already UTF-8.
#include <string>
#include <vector>

#include "core/cli.h"

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return aoi::aoi_main(args);
}
