// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#import <Cocoa/Cocoa.h>
#include <crt_externs.h>

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

}  // namespace torfox::platform
