// Per-user install: no administrator prompt, no MSI, one file.
//
// `aoi install` copies the exe to %LOCALAPPDATA%\Programs\AOI, puts that
// folder on the user's PATH (so `aoi` works in any new terminal), registers
// an "Apps & features" entry so it uninstalls like any app. It never starts
// on its own: `aoi start` / `aoi stop` from a terminal.
#pragma once
#include <filesystem>
#include <string>

namespace aoi {

std::filesystem::path install_dir();
std::filesystem::path installed_exe();
bool is_installed_copy();                  // running from the install location
bool install(std::string *err);
void cleanup_old_copies();
bool uninstall(bool remove_data, std::string *err);
bool autostart_enabled();
void set_autostart(bool on);

}  // namespace aoi
