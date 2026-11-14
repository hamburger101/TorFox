// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#import <Cocoa/Cocoa.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include "src/core/privacy/external_protocols.h"

namespace torfox::platform {

std::filesystem::path DefaultUserDataDir() {
  NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                                 NSUserDomainMask, YES);
  if (dirs.count == 0) {
    return {};
  }
  return std::filesystem::path([dirs[0] fileSystemRepresentation]) / "TorFox";
}

CefRefPtr<CefCommandLine> RawCommandLine() {
  CefRefPtr<CefCommandLine> cl = CefCommandLine::CreateCommandLine();
  cl->InitFromArgv(*_NSGetArgc(), *_NSGetArgv());
  return cl;
}

void ShowFatalError(const std::string& title, const std::string& message) {
  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = [NSString stringWithUTF8String:title.c_str()] ?: @"TorFox";
    alert.informativeText = [NSString stringWithUTF8String:message.c_str()] ?: @"";
    [alert addButtonWithTitle:@"Quit"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
  }
}

void ApplyWindowTheme(CefRefPtr<CefWindow> window, bool dark) {
  if (!window) {
    return;
  }
  // CEF Views hands out the NSView of the window's content; accept an
  // NSWindow too.
  id handle = (__bridge id)window->GetWindowHandle();
  NSWindow* ns_window = nil;
  if ([handle isKindOfClass:[NSWindow class]]) {
    ns_window = handle;
  } else if ([handle isKindOfClass:[NSView class]]) {
    ns_window = [(NSView*)handle window];
  }
  if (!ns_window) {
    return;
  }
  ns_window.appearance =
      [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}

bool SystemPrefersDark() {
  NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
  return [style isEqualToString:@"Dark"];
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(utf8);
}

std::string PathToUtf8(const std::filesystem::path& path) {
  return path.string();
}

bool CopyTextToClipboard(const std::string& utf8) {
  @autoreleasepool {
    NSString* text = [NSString stringWithUTF8String:utf8.c_str()];
    if (!text) {
      return false;
    }
    NSPasteboard* board = [NSPasteboard generalPasteboard];
    [board clearContents];
    return [board setString:text forType:NSPasteboardTypeString];
  }
}


std::filesystem::path DownloadsDir() {
  @autoreleasepool {
    NSArray<NSString*>* dirs =
        NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    if (dirs.count == 0) {
      return {};
    }
    return std::filesystem::path([dirs[0] fileSystemRepresentation]);
  }
}

bool RevealInFolder(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()) {
    return false;
  }
  @autoreleasepool {
    NSString* str = [NSString stringWithUTF8String:PathToUtf8(path).c_str()];
    if (!str) {
      return false;
    }
    // Selects the file in Finder; never opens it.
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:str] ]];
    return true;
  }
}

bool OpenExternal(const std::string& url) {
  if (external_protocols::LaunchableScheme(url).empty()) {
    return false;
  }
  @autoreleasepool {
    NSString* str = [NSString stringWithUTF8String:url.c_str()];
    NSURL* target = str ? [NSURL URLWithString:str] : nil;
    if (!target) {
      return false;
    }
    return [[NSWorkspace sharedWorkspace] openURL:target];
  }
}

namespace {

NSWindow* NativeWindow(CefRefPtr<CefWindow> window) {
  if (!window) {
    return nil;
  }
  id handle = (__bridge id)window->GetWindowHandle();
  if ([handle isKindOfClass:[NSWindow class]]) {
    return handle;
  }
  if ([handle isKindOfClass:[NSView class]]) {
    return [(NSView*)handle window];
  }
  return nil;
}

NSString* ToNs(const std::string& s) {
  NSString* out = [NSString stringWithUTF8String:s.c_str()];
  return out ? out : @"";
}

// Runs on the main thread (CEF's UI thread on macOS), like the sheet itself.
class MacPrompt : public PromptHandle {
 public:
  MacPrompt(NSAlert* alert, NSWindow* parent) : alert_(alert), parent_(parent) {}

  // The first button is Cancel (NSAlertFirstButtonReturn).
  void Cancel() override {
    NSAlert* alert = alert_;
    NSWindow* parent = parent_;
    alert_ = nil;
    if (alert && parent && alert.window.sheetParent == parent) {
      [parent endSheet:alert.window returnCode:NSAlertFirstButtonReturn];
    }
  }

  void Finished() { alert_ = nil; }

 private:
  NSAlert* alert_;
  NSWindow* parent_;
};

}  // namespace

std::shared_ptr<PromptHandle> ShowConfirmPrompt(CefRefPtr<CefWindow> window,
                                                const ConfirmPrompt& prompt,
                                                std::function<void(bool)> done) {
  NSWindow* parent = NativeWindow(window);
  if (!parent || !done) {
    return nullptr;
  }
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = prompt.warning.empty() ? NSAlertStyleInformational : NSAlertStyleWarning;
  alert.messageText = ToNs(prompt.heading);
  std::string info = prompt.details;
  if (!prompt.warning.empty()) {
    info += "\n\n" + prompt.warning;
  }
  alert.informativeText = ToNs(info);
  // NSAlert gives its first button the Return key. Cancel must be the
  // default, so Cancel comes first (Return and Escape both cancel) and the
  // accept button, second, has no key equivalent: it needs a deliberate click.
  NSButton* cancel = [alert addButtonWithTitle:ToNs(prompt.cancel)];
  cancel.keyEquivalent = @"\r";
  NSButton* accept = [alert addButtonWithTitle:ToNs(prompt.accept)];
  accept.keyEquivalent = @"";
  alert.window.title = ToNs(prompt.title);
  auto handle = std::make_shared<MacPrompt>(alert, parent);
  std::weak_ptr<MacPrompt> weak = handle;
  __block std::function<void(bool)> reply = std::move(done);
  [alert beginSheetModalForWindow:parent
                completionHandler:^(NSModalResponse response) {
                  if (auto h = weak.lock()) {
                    h->Finished();
                  }
                  // NSAlertFirstButtonReturn is Cancel; only the second
                  // button (accept) answers true.
                  if (reply) {
                    std::function<void(bool)> f = std::move(reply);
                    reply = nullptr;
                    f(response == NSAlertSecondButtonReturn);
                  }
                }];
  return handle;
}


// ----- Tor --------------------------------------------------------------------

namespace {

class MacChildProcess : public ChildProcess {
 public:
  explicit MacChildProcess(pid_t pid) : pid_(pid) {}
  ~MacChildProcess() override { Kill(); }
  long pid() const override { return static_cast<long>(pid_); }
  bool Exited(int* exit_code) override {
    if (reaped_) {
      if (exit_code) *exit_code = code_;
      return true;
    }
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      reaped_ = true;
      code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (exit_code) *exit_code = code_;
      return true;
    }
    return false;
  }
  bool WaitForExit(int ms) override {
    for (int waited = 0; waited <= ms; waited += 50) {
      if (Exited(nullptr)) {
        return true;
      }
      ::usleep(50 * 1000);
    }
    return Exited(nullptr);
  }
  void Kill() override {
    if (!Exited(nullptr)) {
      ::kill(pid_, SIGTERM);
      if (!WaitForExit(3000)) {
        ::kill(pid_, SIGKILL);
        WaitForExit(2000);
      }
    }
  }

 private:
  pid_t pid_;
  bool reaped_ = false;
  int code_ = 0;
};

}  // namespace

std::unique_ptr<ChildProcess> LaunchHidden(const std::filesystem::path& exe,
                                           const std::vector<std::string>& args,
                                           const std::filesystem::path& working_dir,
                                           std::string* error) {
  std::vector<std::string> storage;
  storage.push_back(exe.string());
  for (const std::string& a : args) storage.push_back(a);
  std::vector<char*> argv;
  for (std::string& a : storage) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addchdir_np(&actions, working_dir.c_str());
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // No file descriptor of TorFox's reaches tor.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), *_NSGetEnviron());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    if (error) *error = "tor could not be started";
    return nullptr;
  }
  return std::make_unique<MacChildProcess>(pid);
}

long CurrentProcessId() {
  return static_cast<long>(::getpid());
}

TorPaths BundledTorPaths() {
  TorPaths p;
  @autoreleasepool {
    // Inside the app bundle: Contents/Helpers/tor (signed with the app).
    std::filesystem::path bundle([[[NSBundle mainBundle] bundlePath] fileSystemRepresentation]);
    p.dir = bundle / "Contents" / "Helpers" / "tor";
  }
  p.exe = p.dir / "tor";
  p.pt_client = "pluggable_transports/lyrebird";
  p.geoip = p.dir / "data" / "geoip";
  p.geoip6 = p.dir / "data" / "geoip6";
  p.pt_config = p.dir / "pluggable_transports" / "pt_config.json";
  return p;
}

std::filesystem::path AsciiPath(const std::filesystem::path& path) {
  for (unsigned char c : path.native()) {
    if (c < 0x20 || c >= 0x7F) {
      return {};  // macOS has no short names; tor gets an error instead.
    }
  }
  return path;
}

}  // namespace torfox::platform
