// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_SHORTCUTS_H_
#define TORFOX_CORE_BROWSING_SHORTCUTS_H_

#include <cstdint>
#include <optional>

namespace torfox::browsing {

// Browser commands reachable from the keyboard, menus and the toolbar.
enum class Command {
  kNewTab,
  kNewWindow,
  kNewPrivateWindow,
  kCloseTab,
  kCloseWindow,
  kReopenClosedTab,
  kFocusAddress,
  kReload,
  kReloadBypassingCache,
  kBack,
  kForward,
  kHome,
  kNextTab,
  kPreviousTab,
  kSelectTab,  // ShortcutCommand::tab_number: 1-8 or 9 for the last tab.
  kMoveTabLeft,
  kMoveTabRight,
  kFind,
  kFindNext,
  kFindPrevious,
  kZoomIn,
  kZoomOut,
  kZoomReset,
  kBookmarkPage,
  kToggleBookmarksBar,
  kShowBookmarks,
  kShowHistory,
  kShowDownloads,
  kSettings,
};

struct ShortcutCommand {
  Command command;
  int tab_number = 0;
};

// Modifier bits (exact match: Ctrl+Shift+T is not Ctrl+T).
namespace keymod {
inline constexpr uint32_t kShift = 1;
inline constexpr uint32_t kCtrl = 2;
inline constexpr uint32_t kAlt = 4;  // Option on macOS.
inline constexpr uint32_t kCmd = 8;
}  // namespace keymod

enum class KeyPlatform { kWindows, kMac };

// Maps a key-down (Windows virtual-key code, as CEF reports on every
// platform) to a browser command, following each platform's browser
// conventions. Keys that edit text (Cmd+Left on macOS, Backspace) are never
// taken from the page.
std::optional<ShortcutCommand> MapShortcut(int key_code, uint32_t mods, KeyPlatform platform);

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_SHORTCUTS_H_
