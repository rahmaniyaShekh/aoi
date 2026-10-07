#include "install.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>

#include "util.h"

namespace aoi {

namespace {
const wchar_t *kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AOI";
const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::filesystem::path local_appdata() {
  PWSTR p = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}

void set_str(HKEY k, const wchar_t *name, const std::wstring &v, DWORD type = REG_SZ) {
  RegSetValueExW(k, name, 0, type, reinterpret_cast<const BYTE *>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}
void set_dw(HKEY k, const wchar_t *name, DWORD v) {
  RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&v), sizeof v);
}

std::wstring lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), ::towlower);
  while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  return s;
}

// Adds or removes our folder on the *user* PATH (HKCU\Environment), then
// tells Explorer so new terminals see it without logging out.
void edit_user_path(bool add) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
  wchar_t buf[32767];
  DWORD sz = sizeof buf, type = REG_EXPAND_SZ;
  std::wstring path;
  if (RegQueryValueExW(k, L"Path", nullptr, &type, reinterpret_cast<BYTE *>(buf), &sz) == ERROR_SUCCESS)
    path.assign(buf, sz / sizeof(wchar_t));
  while (!path.empty() && path.back() == L'\0') path.pop_back();
  std::wstring dir = install_dir().wstring();
  std::vector<std::wstring> parts;
  size_t start = 0;
  while (start <= path.size()) {
    size_t e = path.find(L';', start);
    if (e == std::wstring::npos) e = path.size();
    std::wstring p = path.substr(start, e - start);
    if (!p.empty() && lower(p) != lower(dir)) parts.push_back(p);
    start = e + 1;
  }
  if (add) parts.push_back(dir);
  std::wstring out;
  for (auto &p : parts) { if (!out.empty()) out += L';'; out += p; }
  set_str(k, L"Path", out, type == REG_SZ ? REG_SZ : REG_EXPAND_SZ);
  RegCloseKey(k);
  DWORD_PTR r;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG,
                      2000, &r);
}
}  // namespace

// Removes aoi.exe.old* left by earlier updates (they are locked only while the
// process that was running from them is alive).
void cleanup_old_copies() {
  std::error_code ec;
  for (auto &f : std::filesystem::directory_iterator(install_dir(), ec)) {
    auto name = f.path().filename().wstring();
    if (name.rfind(L"aoi.exe.old", 0) == 0) DeleteFileW(f.path().c_str());
  }
}

std::filesystem::path install_dir() { return local_appdata() / L"Programs" / L"AOI"; }
std::filesystem::path installed_exe() { return install_dir() / L"aoi.exe"; }

bool is_installed_copy() {
  std::error_code ec;
  return std::filesystem::equivalent(exe_path(), installed_exe(), ec);
}

bool install(std::string *err) {
  std::error_code ec;
  std::filesystem::create_directories(install_dir(), ec);
  if (!is_installed_copy()) {
    // A running copy (the host just stopped, or the `aoi update` that launched
    // us) locks its file against overwriting, but it can be renamed out of the
    // way. Each rename gets a unique name so a leftover, still-locked .old file
    // can never block it, and both steps retry briefly: a process that has just
    // exited, or an antivirus scan, can hold the file for a moment.
    cleanup_old_copies();
    std::filesystem::path old;
    DWORD e = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 25 && !ok; ++attempt) {
      if (attempt) Sleep(200);
      if (std::filesystem::exists(installed_exe(), ec)) {
        old = installed_exe();
        old += L".old-" + widen(random_hex(4));
        if (!MoveFileExW(installed_exe().c_str(), old.c_str(), 0)) { e = GetLastError(); continue; }
      }
      ok = CopyFileW(exe_path().c_str(), installed_exe().c_str(), FALSE) != 0;
      if (!ok) {
        e = GetLastError();
        if (!old.empty()) MoveFileExW(old.c_str(), installed_exe().c_str(), 0);  // put it back and retry
      }
    }
    if (!ok) {
      if (err) *err = "could not copy to " + narrow(install_dir().wstring()) + ": " + hr_text(long(HRESULT_FROM_WIN32(e)));
      return false;
    }
    if (!old.empty() && !DeleteFileW(old.c_str())) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
  edit_user_path(true);

  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    std::wstring exe = installed_exe().wstring();
    set_str(k, L"DisplayName", L"AOI - Audio Over Internet");
    set_str(k, L"DisplayVersion", widen(AOI_VERSION));
    set_str(k, L"Publisher", L"mdarif.online");
    set_str(k, L"DisplayIcon", exe);
    set_str(k, L"InstallLocation", install_dir().wstring());
    set_str(k, L"UninstallString", L"\"" + exe + L"\" uninstall");
    set_str(k, L"URLInfoAbout", L"https://share.mdarif.online/aoi");
    set_dw(k, L"NoModify", 1);
    set_dw(k, L"NoRepair", 1);
    auto sz = std::filesystem::file_size(installed_exe(), ec);
    set_dw(k, L"EstimatedSize", DWORD(sz / 1024));
    RegCloseKey(k);
  }
  // Started and stopped from the terminal only: never at sign-in. (Clears an
  // entry an earlier build may have left.)
  set_autostart(false);
  LOGI("installed to %s", narrow(install_dir().wstring()).c_str());
  return true;
}

bool uninstall(bool remove_data, std::string *err) {
  set_autostart(false);
  edit_user_path(false);
  RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
  std::error_code ec;
  if (remove_data) std::filesystem::remove_all(data_dir(), ec);
  // A running exe cannot delete itself: a hidden shell removes the folder
  // a moment after we exit.
  std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + install_dir().wstring() + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                     &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else if (err) {
    *err = "could not schedule removal of the program folder";
  }
  return true;
}

bool autostart_enabled() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  bool on = RegQueryValueExW(k, L"AOI", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(k);
  return on;
}

void set_autostart(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
  if (on) {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::exists(installed_exe(), ec) ? installed_exe() : exe_path();
    set_str(k, L"AOI", L"\"" + exe.wstring() + L"\" daemon");
  } else {
    RegDeleteValueW(k, L"AOI");
  }
  RegCloseKey(k);
}

}  // namespace aoi
