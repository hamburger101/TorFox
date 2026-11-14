// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#import <Cocoa/Cocoa.h>
#include <crt_externs.h>

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

}  // namespace torfox::platform
