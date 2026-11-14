// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#include <windows.h>

#include <dwmapi.h>
#include <shlobj.h>

namespace torfox::platform {

namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return std::wstring();
  }
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) {
    return std::string();
  }
  int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

}  // namespace

std::filesystem::path DefaultUserDataDir() {
  PWSTR raw = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) ||
      !raw) {
    return {};
  }
  std::filesystem::path dir(raw);
  ::CoTaskMemFree(raw);
  return dir / L"TorFox";
}

CefRefPtr<CefCommandLine> RawCommandLine() {
  CefRefPtr<CefCommandLine> cl = CefCommandLine::CreateCommandLine();
  cl->InitFromString(::GetCommandLineW());
  return cl;
}

void ShowFatalError(const std::string& title, const std::string& message) {
  ::MessageBoxW(nullptr, Widen(message).c_str(), Widen(title).c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
}

void ApplyWindowTheme(CefRefPtr<CefWindow> window, bool dark) {
  if (!window) {
    return;
  }
  HWND hwnd = window->GetWindowHandle();
  if (!hwnd) {
    return;
  }
  // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+ and Windows 11).
  constexpr DWORD kUseImmersiveDarkMode = 20;
  BOOL value = dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof(value));
}

bool SystemPrefersDark() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  LSTATUS status = ::RegGetValueW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return status == ERROR_SUCCESS && value == 0;
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(Widen(utf8));
}

std::string PathToUtf8(const std::filesystem::path& path) {
  return Narrow(path.native());
}

}  // namespace torfox::platform
