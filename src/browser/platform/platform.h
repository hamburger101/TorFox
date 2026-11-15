// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_PLATFORM_PLATFORM_H_
#define TORFOX_BROWSER_PLATFORM_PLATFORM_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

// Puts |utf8| on the system clipboard as plain text. Returns false on failure.
bool CopyTextToClipboard(const std::string& utf8);

// The user's Downloads folder (Windows: the Downloads known folder; macOS:
// ~/Downloads). Empty on failure.
std::filesystem::path DownloadsDir();

// Opens the file manager with |path| selected (Explorer / Finder), without
// opening the file itself. Returns false if it could not be started.
bool RevealInFolder(const std::filesystem::path& path);

// Hands |url| to the app the user chose for its scheme (the mail or phone
// app). Only URLs external_protocols::LaunchableScheme accepts are passed
// on; anything else is refused. Returns false if it was refused or could
// not be started.
bool OpenExternal(const std::string& url);

// A yes/no question for a native dialog. Cancel is the default button.
struct ConfirmPrompt {
  std::string title;    // Dialog title, e.g. "Download this file?"
  std::string heading;  // Main line, e.g. the file name
  std::string details;  // Further lines (size, origin)
  std::string warning;  // Optional, shown with a warning icon
  std::string accept;   // Accept button label
  std::string cancel;   // Cancel button label
};

class PromptHandle {
 public:
  virtual ~PromptHandle() = default;
  // Closes the prompt as if Cancel had been chosen. Safe to call more than
  // once, and after the prompt has been answered.
  virtual void Cancel() = 0;
};

// Shows |prompt| as a window-modal native dialog without blocking the UI
// thread (Windows: TaskDialog on its own thread; macOS: NSAlert sheet).
// |done| runs exactly once, on the UI thread, with true only if the user
// chose |accept|. Returns null, without calling |done|, if no dialog could be
// shown; the caller must then treat the prompt as cancelled.
std::shared_ptr<PromptHandle> ShowConfirmPrompt(CefRefPtr<CefWindow> window,
                                                const ConfirmPrompt& prompt,
                                                std::function<void(bool)> done);

// UTF-8 <-> native path helpers.
std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);

// ----- Tor --------------------------------------------------------------------

// A program TorFox started (tor). It cannot outlive TorFox: on Windows it
// runs in a job object that kills it when TorFox exits, however TorFox ends;
// on macOS tor's own __OwningControllerProcess check ends it.
class ChildProcess {
 public:
  virtual ~ChildProcess() = default;  // Kills the process if it is running.
  virtual long pid() const = 0;
  // True once the process has exited, with its |exit_code|.
  virtual bool Exited(int* exit_code) = 0;
  // Waits up to |ms| milliseconds for it to exit.
  virtual bool WaitForExit(int ms) = 0;
  virtual void Kill() = 0;
};

// Starts |exe| with |args| (UTF-8) in |working_dir|, without a console
// window, with no handles inherited. Null, with |error|, on failure.
std::unique_ptr<ChildProcess> LaunchHidden(const std::filesystem::path& exe,
                                           const std::vector<std::string>& args,
                                           const std::filesystem::path& working_dir,
                                           std::string* error);

long CurrentProcessId();

// Where the bundled tor lives. |dir| is tor's working directory: it holds
// the tor executable and pluggable_transports/; |pt_client| is lyrebird's
// path relative to it.
struct TorPaths {
  std::filesystem::path dir;
  std::filesystem::path exe;
  std::string pt_client;
  std::filesystem::path geoip;
  std::filesystem::path geoip6;
  std::filesystem::path pt_config;
};
TorPaths BundledTorPaths();

// |path| (which must exist) in a form torrc can carry: unchanged if it is
// ASCII; on Windows its short (8.3) form otherwise. Empty if neither works.
std::filesystem::path AsciiPath(const std::filesystem::path& path);

// ----- Updates ------------------------------------------------------------------

// Marks this process as a running TorFox browser until it exits, so that the
// installer can wait for TorFox to close before it replaces its files.
//   Windows: a named mutex in this session ("TorFoxBrowserRunning"), held
//            until the process exits (packaging/windows/TorFox.iss).
//   macOS:   nothing (the user replaces the app).
void MarkRunning();

// This build's entry in a release manifest: "windows-x64", "macos-arm64" or
// "macos-x64".
const char* UpdatePlatform();

// Hands a downloaded update over, after TorFox has shut down. The file is
// opened so that it cannot be changed or replaced, hashed again through that
// handle and only then started, while the handle is still held.
//   Windows: runs the installer (it asks for administrator rights, waits for
//            TorFox to exit and installs silently).
//   macOS:   opens the disk image in Finder; the user copies TorFox to
//            Applications (TorFox does not replace itself on macOS).
bool LaunchUpdate(const std::filesystem::path& file, const std::string& sha256, uint64_t size,
                  std::string* error);

}  // namespace torfox::platform

#endif  // TORFOX_BROWSER_PLATFORM_PLATFORM_H_
