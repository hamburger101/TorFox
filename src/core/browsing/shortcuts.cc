// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/shortcuts.h"

namespace torfox::browsing {

namespace {

// Windows virtual-key codes.
constexpr int kTab = 0x09;
constexpr int kPageUp = 0x21;
constexpr int kPageDown = 0x22;
constexpr int kHomeKey = 0x24;
constexpr int kLeft = 0x25;
constexpr int kRight = 0x27;
constexpr int kNumpad0 = 0x60;
constexpr int kNumpad1 = 0x61;
constexpr int kNumpad9 = 0x69;
constexpr int kNumpadAdd = 0x6B;
constexpr int kNumpadSubtract = 0x6D;
constexpr int kF3 = 0x72;
constexpr int kF4 = 0x73;
constexpr int kF5 = 0x74;
constexpr int kF6 = 0x75;
constexpr int kEquals = 0xBB;  // "=" / "+"
constexpr int kComma = 0xBC;
constexpr int kMinus = 0xBD;
constexpr int kOpenBracket = 0xDB;
constexpr int kCloseBracket = 0xDD;

using namespace keymod;

std::optional<ShortcutCommand> Cmd(Command c, int tab = 0) {
  return ShortcutCommand{c, tab};
}

std::optional<int> TabNumber(int key) {
  if (key >= '1' && key <= '9') {
    return key - '0';
  }
  if (key >= kNumpad1 && key <= kNumpad9) {
    return key - kNumpad1 + 1;
  }
  return std::nullopt;
}

// Shared by both platforms, with |p| as the primary modifier (Ctrl or Cmd).
std::optional<ShortcutCommand> Common(int key, uint32_t mods, uint32_t p) {
  if (mods == p) {
    switch (key) {
      case 'T':
        return Cmd(Command::kNewTab);
      case 'N':
        return Cmd(Command::kNewWindow);
      case 'W':
        return Cmd(Command::kCloseTab);
      case 'L':
        return Cmd(Command::kFocusAddress);
      case 'R':
        return Cmd(Command::kReload);
      case 'F':
        return Cmd(Command::kFind);
      case 'G':
        return Cmd(Command::kFindNext);
      case 'D':
        return Cmd(Command::kBookmarkPage);
      case kEquals:
      case kNumpadAdd:
        return Cmd(Command::kZoomIn);
      case kMinus:
      case kNumpadSubtract:
        return Cmd(Command::kZoomOut);
      case '0':
      case kNumpad0:
        return Cmd(Command::kZoomReset);
      case kComma:
        return Cmd(Command::kSettings);
    }
    if (auto n = TabNumber(key)) {
      return Cmd(Command::kSelectTab, *n);
    }
  }
  if (mods == (p | kShift)) {
    switch (key) {
      case 'N':
        return Cmd(Command::kNewPrivateWindow);
      case 'W':
        return Cmd(Command::kCloseWindow);
      case 'T':
        return Cmd(Command::kReopenClosedTab);
      case 'R':
        return Cmd(Command::kReloadBypassingCache);
      case 'G':
        return Cmd(Command::kFindPrevious);
      case 'B':
        return Cmd(Command::kToggleBookmarksBar);
      case kEquals:  // Ctrl/Cmd and "+" on layouts where "+" needs Shift.
        return Cmd(Command::kZoomIn);
    }
  }
  if (mods == kCtrl && key == kTab) {
    return Cmd(Command::kNextTab);
  }
  if (mods == (kCtrl | kShift) && key == kTab) {
    return Cmd(Command::kPreviousTab);
  }
  return std::nullopt;
}

std::optional<ShortcutCommand> Windows(int key, uint32_t mods) {
  if (auto c = Common(key, mods, kCtrl)) {
    return c;
  }
  if (mods == kCtrl) {
    switch (key) {
      case kF4:
        return Cmd(Command::kCloseTab);
      case 'K':
      case 'E':
        return Cmd(Command::kFocusAddress);
      case kF5:
        return Cmd(Command::kReloadBypassingCache);
      case kPageDown:
        return Cmd(Command::kNextTab);
      case kPageUp:
        return Cmd(Command::kPreviousTab);
      case 'H':
        return Cmd(Command::kShowHistory);
      case 'J':
        return Cmd(Command::kShowDownloads);
    }
  }
  if (mods == (kCtrl | kShift)) {
    switch (key) {
      case kPageUp:
        return Cmd(Command::kMoveTabLeft);
      case kPageDown:
        return Cmd(Command::kMoveTabRight);
      case 'O':
        return Cmd(Command::kShowBookmarks);
    }
  }
  if (mods == kAlt) {
    switch (key) {
      case kLeft:
        return Cmd(Command::kBack);
      case kRight:
        return Cmd(Command::kForward);
      case kHomeKey:
        return Cmd(Command::kHome);
      case 'D':
        return Cmd(Command::kFocusAddress);
    }
  }
  if (mods == 0) {
    switch (key) {
      case kF3:
        return Cmd(Command::kFindNext);
      case kF5:
        return Cmd(Command::kReload);
      case kF6:
        return Cmd(Command::kFocusAddress);
    }
  }
  if (mods == kShift) {
    switch (key) {
      case kF3:
        return Cmd(Command::kFindPrevious);
      case kF5:
        return Cmd(Command::kReloadBypassingCache);
    }
  }
  return std::nullopt;
}

std::optional<ShortcutCommand> Mac(int key, uint32_t mods) {
  if (auto c = Common(key, mods, kCmd)) {
    return c;
  }
  if (mods == kCmd) {
    switch (key) {
      case kOpenBracket:
        return Cmd(Command::kBack);
      case kCloseBracket:
        return Cmd(Command::kForward);
      case 'Y':
        return Cmd(Command::kShowHistory);
    }
  }
  if (mods == (kCmd | kShift)) {
    switch (key) {
      case kOpenBracket:
        return Cmd(Command::kPreviousTab);
      case kCloseBracket:
        return Cmd(Command::kNextTab);
      case 'H':
        return Cmd(Command::kHome);
    }
  }
  if (mods == (kCmd | kAlt)) {
    switch (key) {
      case kLeft:
        return Cmd(Command::kPreviousTab);
      case kRight:
        return Cmd(Command::kNextTab);
      case 'B':
        return Cmd(Command::kShowBookmarks);
      case 'L':
        return Cmd(Command::kShowDownloads);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<ShortcutCommand> MapShortcut(int key_code, uint32_t mods, KeyPlatform platform) {
  return platform == KeyPlatform::kMac ? Mac(key_code, mods) : Windows(key_code, mods);
}

}  // namespace torfox::browsing
