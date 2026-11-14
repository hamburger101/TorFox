// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_PLATFORM_PLATFORM_H_
#define TORFOX_BROWSER_PLATFORM_PLATFORM_H_

#include <filesystem>
#include <string>

#include "include/cef_command_line.h"
#include "include/views/cef_window.h"

// Small, per-OS services. Implemented in platform_win.cc and platform_mac.mm.
namespace torfox::platform {

// Per-user, non-roaming data directory:
//   Windows: %LOCALAPPDATA%\TorFox
//   macOS:   ~/Library/Application Support/TorFox
// Empty on failure.
std::filesystem::path DefaultUserDataDir();

// The command line exactly as the OS delivered it to this process.
CefRefPtr<CefCommandLine> RawCommandLine();

// Blocking native error dialog, used when TorFox refuses to start.
void ShowFatalError(const std::string& title, const std::string& message);

// Native window chrome for the chosen theme (Windows: dark title bar via DWM;
// macOS: NSAppearance).
void ApplyWindowTheme(CefRefPtr<CefWindow> window, bool dark);

// True if the OS is set to a dark app theme.
bool SystemPrefersDark();

// UTF-8 <-> native path helpers.
std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace torfox::platform

#endif  // TORFOX_BROWSER_PLATFORM_PLATFORM_H_
