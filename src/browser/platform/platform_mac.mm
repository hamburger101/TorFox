// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#import <Cocoa/Cocoa.h>
#include <crt_externs.h>
#include <arpa/inet.h>
#include <libproc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <set>
#include <thread>
#include <utility>

#include "src/core/privacy/external_protocols.h"
#include "src/core/update/sha256.h"

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

void MarkRunning() {}

const char* UpdatePlatform() {
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-arm64";
#else
  return "macos-x64";
#endif
}

bool InstallsUpdatesOnExit() {
  return false;
}

bool SessionEnding() {
  return false;
}

bool UpdateBeingInstalled() {
  return false;
}

bool RequestStartAfterUpdate() {
  return false;
}

void ClearStartAfterUpdate() {}

void HoldUpdateInstalling() {}

void ReleaseUpdateInstalling() {}

// |relaunch| does not apply: the user copies the new app and starts it.
bool LaunchUpdate(const std::filesystem::path& file, const std::string& sha256, uint64_t size,
                  bool relaunch, std::string* error) {
  (void)relaunch;
  // macOS has no mandatory locks: the disk image is checked again just
  // before Finder opens it, and macOS checks the image itself when it mounts.
  const std::optional<update::FileDigest> d = update::HashFile(file, size);
  if (!d || d->size != size || d->sha256 != sha256) {
    *error = "the update changed after it was checked";
    return false;
  }
  @autoreleasepool {
    NSString* str = [NSString stringWithUTF8String:PathToUtf8(file).c_str()];
    if (!str || ![[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:str]]) {
      *error = "the disk image could not be opened";
      return false;
    }
  }
  return true;
}

long CurrentProcessId() {
  return static_cast<long>(::getpid());
}

namespace {

// The physical footprint: what Activity Monitor's Memory column shows.
uint64_t Footprint(pid_t pid) {
  rusage_info_v2 info = {};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&info)) != 0) {
    return 0;
  }
  return info.ri_phys_footprint;
}

std::string ProcessPath(pid_t pid) {
  char buf[PROC_PIDPATHINFO_MAXSIZE];
  const int n = proc_pidpath(pid, buf, sizeof buf);
  return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

bool InFolder(const std::string& path, const std::string& folder) {
  return !folder.empty() && path.size() > folder.size() + 1 &&
         path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}

std::vector<pid_t> ChildrenOf(pid_t pid) {
  std::vector<pid_t> buf(1024);
  const int bytes = proc_listpids(PROC_PPID_ONLY, static_cast<uint32_t>(pid), buf.data(),
                                  static_cast<int>(buf.size() * sizeof(pid_t)));
  if (bytes <= 0) {
    return {};
  }
  buf.resize(std::min(buf.size(), static_cast<std::size_t>(bytes) / sizeof(pid_t)));
  buf.erase(std::remove(buf.begin(), buf.end(), 0), buf.end());
  return buf;
}

}  // namespace

// Untested: macOS is built and tested only on CI (docs/FEATURE_STATUS.md).
// An orphaned process is adopted by launchd, so a parent ID always names the
// live parent.
int ReservedLoopbackPort() {
  static const int port = [] {
    // Kept open until TorFox exits. No SO_REUSEADDR or SO_REUSEPORT: no
    // other socket can take 127.0.0.1 on this port, and an incoming
    // connection matches this listener before any wildcard one.
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
      return 0;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    sockaddr_in addr = {};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        listen(s, SOMAXCONN) != 0) {
      close(s);
      return 0;
    }
    // Each connection is reset as soon as it arrives, before anything is
    // read from it.
    std::thread([s] {
      for (;;) {
        const int c = accept(s, nullptr, nullptr);
        if (c < 0) {
          if (errno == EBADF || errno == EINVAL) {
            return;
          }
          usleep(50 * 1000);
          continue;
        }
        linger reset = {1, 0};
        setsockopt(c, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        close(c);
      }
    }).detach();
    return static_cast<int>(ntohs(addr.sin_port));
  }();
  return port;
}

std::vector<memory::Process> TorFoxProcesses(const std::filesystem::path& tor_dir) {
  std::vector<memory::Process> out;
  std::string bundle;
  @autoreleasepool {
    const char* path = [[[NSBundle mainBundle] bundlePath] fileSystemRepresentation];
    bundle = path ? path : "";
  }
  if (bundle.empty()) {
    return out;
  }
  const std::string tor = tor_dir.string();
  const pid_t self = ::getpid();
  out.push_back({static_cast<long>(self), memory::Process::Kind::kBrowser, Footprint(self)});
  std::vector<pid_t> todo = ChildrenOf(self);
  std::set<pid_t> seen = {self};
  while (!todo.empty() && out.size() < 1024) {
    const pid_t pid = todo.back();
    todo.pop_back();
    if (!seen.insert(pid).second) {
      continue;
    }
    const std::string path = ProcessPath(pid);
    memory::Process::Kind kind = memory::Process::Kind::kHelper;
    if (InFolder(path, tor)) {
      kind = memory::Process::Kind::kTor;
    } else if (!InFolder(path, bundle)) {
      continue;  // Not TorFox's.
    }
    out.push_back({static_cast<long>(pid), kind, Footprint(pid)});
    for (pid_t child : ChildrenOf(pid)) {
      todo.push_back(child);
    }
  }
  return out;
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
