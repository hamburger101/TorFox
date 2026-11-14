// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// macOS browser process entry point (TorFox.app/Contents/MacOS/TorFox).
// Sub-processes run in the helper bundles instead (mac_helper.cc), each of
// which initialises the macOS sandbox before anything else.

#import <Cocoa/Cocoa.h>

#include "include/cef_application_mac.h"
#include "include/wrapper/cef_library_loader.h"
#include "src/browser/app/browser_app.h"

// Chromium needs the NSApplication to implement CefAppProtocol so it knows
// when an event is being dispatched.
@interface TorFoxApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation TorFoxApplication

- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

// Cmd+Q and the application menu's Quit: close every window the normal way,
// so each tab shuts down cleanly. TorFox quits when its last window closes.
- (void)terminate:(id)sender {
  for (NSWindow* window in [[self windows] copy]) {
    [window performClose:nil];
  }
}

@end

namespace {

NSMenuItem* Item(NSString* title, SEL action, NSString* key, NSEventModifierFlags mods) {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  item.keyEquivalentModifierMask = mods;
  return item;
}

// The standard macOS menus. The Edit menu matters beyond looks: without it,
// Cmd+C, Cmd+V and friends do nothing in text fields. TorFox's own commands
// (new tab, close tab, settings) are handled as keyboard shortcuts by the
// browser for now; they move into these menus with the platform work.
void InstallMainMenu() {
  NSString* name = @"TorFox";
  NSMenu* bar = [[NSMenu alloc] init];
  const NSEventModifierFlags cmd = NSEventModifierFlagCommand;

  NSMenu* app = [[NSMenu alloc] initWithTitle:name];
  [app addItem:Item([@"Hide " stringByAppendingString:name], @selector(hide:), @"h", cmd)];
  [app addItem:Item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                    cmd | NSEventModifierFlagOption)];
  [app addItem:Item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItem:Item([@"Quit " stringByAppendingString:name], @selector(terminate:), @"q", cmd)];

  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItem:Item(@"Undo", @selector(undo:), @"z", cmd)];
  [edit addItem:Item(@"Redo", @selector(redo:), @"z", cmd | NSEventModifierFlagShift)];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:Item(@"Cut", @selector(cut:), @"x", cmd)];
  [edit addItem:Item(@"Copy", @selector(copy:), @"c", cmd)];
  [edit addItem:Item(@"Paste", @selector(paste:), @"v", cmd)];
  [edit addItem:Item(@"Select All", @selector(selectAll:), @"a", cmd)];

  NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
  [window addItem:Item(@"Minimize", @selector(performMiniaturize:), @"m", cmd)];
  [window addItem:Item(@"Zoom", @selector(performZoom:), @"", 0)];

  for (NSMenu* menu in @[ app, edit, window ]) {
    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:menu.title action:nil keyEquivalent:@""];
    holder.submenu = menu;
    [bar addItem:holder];
  }
  [NSApp setMainMenu:bar];
  [NSApp setWindowsMenu:window];
}

}  // namespace

int main(int argc, char* argv[]) {
  // The CEF framework is loaded at run time, as the macOS sandbox requires.
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInMain()) {
    return 1;
  }
  int code = 0;
  @autoreleasepool {
    [TorFoxApplication sharedApplication];
    // CefAppProtocol is mandatory; refuse to run with any other NSApplication.
    if (![NSApp conformsToProtocol:@protocol(CefAppProtocol)]) {
      return 1;
    }
    InstallMainMenu();
    CefMainArgs main_args(argc, argv);
    code = torfox::TorFoxMain(main_args, nullptr);
  }
  return code;
}
