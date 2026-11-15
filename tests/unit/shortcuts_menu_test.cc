// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "gtest/gtest.h"
#include "src/core/browsing/context_menu.h"
#include "src/core/browsing/shortcuts.h"

namespace torfox::browsing {
namespace {

using namespace keymod;

std::optional<Command> Win(int key, uint32_t mods) {
  auto c = MapShortcut(key, mods, KeyPlatform::kWindows);
  return c ? std::optional<Command>(c->command) : std::nullopt;
}

std::optional<Command> Mac(int key, uint32_t mods) {
  auto c = MapShortcut(key, mods, KeyPlatform::kMac);
  return c ? std::optional<Command>(c->command) : std::nullopt;
}

TEST(Shortcuts, WindowsBrowserConventions) {
  EXPECT_EQ(Win('T', kCtrl), Command::kNewTab);
  EXPECT_EQ(Win('N', kCtrl), Command::kNewWindow);
  EXPECT_EQ(Win('N', kCtrl | kShift), Command::kNewPrivateWindow);
  EXPECT_EQ(Win('W', kCtrl), Command::kCloseTab);
  EXPECT_EQ(Win(0x73, kCtrl), Command::kCloseTab);  // Ctrl+F4
  EXPECT_EQ(Win('W', kCtrl | kShift), Command::kCloseWindow);
  EXPECT_EQ(Win('T', kCtrl | kShift), Command::kReopenClosedTab);
  EXPECT_EQ(Win('J', kCtrl), Command::kShowDownloads);
  EXPECT_EQ(Win('L', kCtrl), Command::kFocusAddress);
  EXPECT_EQ(Win('D', kAlt), Command::kFocusAddress);
  EXPECT_EQ(Win(0x75, 0), Command::kFocusAddress);  // F6
  EXPECT_EQ(Win(0x74, 0), Command::kReload);        // F5
  EXPECT_EQ(Win(0x74, kCtrl), Command::kReloadBypassingCache);
  EXPECT_EQ(Win(0x74, kShift), Command::kReloadBypassingCache);
  EXPECT_EQ(Win('R', kCtrl | kShift), Command::kReloadBypassingCache);
  EXPECT_EQ(Win(0x25, kAlt), Command::kBack);
  EXPECT_EQ(Win(0x27, kAlt), Command::kForward);
  EXPECT_EQ(Win(0x24, kAlt), Command::kHome);
  EXPECT_EQ(Win(0x09, kCtrl), Command::kNextTab);
  EXPECT_EQ(Win(0x09, kCtrl | kShift), Command::kPreviousTab);
  EXPECT_EQ(Win(0x22, kCtrl), Command::kNextTab);
  EXPECT_EQ(Win(0x21, kCtrl), Command::kPreviousTab);
  EXPECT_EQ(Win(0x21, kCtrl | kShift), Command::kMoveTabLeft);
  EXPECT_EQ(Win(0x22, kCtrl | kShift), Command::kMoveTabRight);
  EXPECT_EQ(Win('F', kCtrl), Command::kFind);
  EXPECT_EQ(Win(0x72, 0), Command::kFindNext);  // F3
  EXPECT_EQ(Win(0x72, kShift), Command::kFindPrevious);
  EXPECT_EQ(Win('G', kCtrl), Command::kFindNext);
  EXPECT_EQ(Win('G', kCtrl | kShift), Command::kFindPrevious);
  EXPECT_EQ(Win(0xBB, kCtrl), Command::kZoomIn);
  EXPECT_EQ(Win(0xBB, kCtrl | kShift), Command::kZoomIn);
  EXPECT_EQ(Win(0x6B, kCtrl), Command::kZoomIn);
  EXPECT_EQ(Win(0xBD, kCtrl), Command::kZoomOut);
  EXPECT_EQ(Win(0x6D, kCtrl), Command::kZoomOut);
  EXPECT_EQ(Win('0', kCtrl), Command::kZoomReset);
  EXPECT_EQ(Win(0x60, kCtrl), Command::kZoomReset);
  EXPECT_EQ(Win('D', kCtrl), Command::kBookmarkPage);
  EXPECT_EQ(Win('B', kCtrl | kShift), Command::kToggleBookmarksBar);
  EXPECT_EQ(Win('O', kCtrl | kShift), Command::kShowBookmarks);
  EXPECT_EQ(Win('H', kCtrl), Command::kShowHistory);
  EXPECT_EQ(Win(0xBC, kCtrl), Command::kSettings);
}

TEST(Shortcuts, TabNumbers) {
  auto one = MapShortcut('1', kCtrl, KeyPlatform::kWindows);
  ASSERT_TRUE(one);
  EXPECT_EQ(one->command, Command::kSelectTab);
  EXPECT_EQ(one->tab_number, 1);
  EXPECT_EQ(MapShortcut('9', kCtrl, KeyPlatform::kWindows)->tab_number, 9);
  EXPECT_EQ(MapShortcut(0x63, kCtrl, KeyPlatform::kWindows)->tab_number, 3);  // Numpad 3
  EXPECT_EQ(MapShortcut('4', kCmd, KeyPlatform::kMac)->tab_number, 4);
  EXPECT_FALSE(MapShortcut('1', kAlt, KeyPlatform::kWindows));
}

TEST(Shortcuts, ModifiersMustMatchExactly) {
  EXPECT_FALSE(Win('T', 0));
  EXPECT_FALSE(Win('T', kCtrl | kAlt));
  EXPECT_FALSE(Win('T', kCmd));          // Cmd is not Ctrl on Windows.
  EXPECT_FALSE(Win('L', kCtrl | kAlt));  // AltGr+L on some layouts.
  EXPECT_FALSE(Win('A', kCtrl));         // Select all stays with the page.
  EXPECT_FALSE(Win('C', kCtrl));
  EXPECT_FALSE(Win('V', kCtrl));
  EXPECT_FALSE(Win(0x08, 0));            // Backspace is never "Back".
  EXPECT_FALSE(Win(0x25, kCtrl));        // Ctrl+Left moves by word.
}

TEST(Shortcuts, MacConventions) {
  EXPECT_EQ(Mac('T', kCmd), Command::kNewTab);
  EXPECT_EQ(Mac('N', kCmd | kShift), Command::kNewPrivateWindow);
  EXPECT_EQ(Mac('T', kCmd | kShift), Command::kReopenClosedTab);
  EXPECT_EQ(Mac('L', kCmd | kAlt), Command::kShowDownloads);
  EXPECT_EQ(Mac(0xDB, kCmd), Command::kBack);
  EXPECT_EQ(Mac(0xDD, kCmd), Command::kForward);
  EXPECT_EQ(Mac(0xDB, kCmd | kShift), Command::kPreviousTab);
  EXPECT_EQ(Mac(0xDD, kCmd | kShift), Command::kNextTab);
  EXPECT_EQ(Mac(0x27, kCmd | kAlt), Command::kNextTab);
  EXPECT_EQ(Mac('H', kCmd | kShift), Command::kHome);
  EXPECT_EQ(Mac('Y', kCmd), Command::kShowHistory);
  EXPECT_EQ(Mac('B', kCmd | kAlt), Command::kShowBookmarks);
  EXPECT_EQ(Mac('F', kCmd), Command::kFind);
  EXPECT_EQ(Mac('G', kCmd | kShift), Command::kFindPrevious);
  EXPECT_EQ(Mac(0x09, kCtrl), Command::kNextTab);
  // Text editing keys stay with the page.
  EXPECT_FALSE(Mac(0x25, kCmd));  // Cmd+Left: start of line.
  EXPECT_FALSE(Mac(0x27, kCmd));
  EXPECT_FALSE(Mac('T', kCtrl));  // Ctrl is not the primary modifier on macOS.
  EXPECT_FALSE(Mac(0x74, 0));     // F5 is not reload on macOS.
}

TEST(ContextMenu, SearchesOrdinarySelections) {
  EXPECT_EQ(SelectionSearchUrl("privacy browser"),
            "https://duckduckgo.com/?q=privacy%20browser");
  // Whitespace (including line breaks) collapses.
  EXPECT_EQ(SelectionSearchUrl("  two\n\tlines  "), "https://duckduckgo.com/?q=two%20lines");
}

TEST(ContextMenu, NeverSearchesOnionAddresses) {
  const std::string onion = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";
  EXPECT_FALSE(SelectionSearchUrl(onion));
  EXPECT_FALSE(SelectionSearchUrl("visit " + onion + " today"));
  EXPECT_FALSE(SelectionSearchUrl("http://" + onion + "/path"));
  EXPECT_FALSE(SelectionSearchUrl("ABC.ONION"));
  EXPECT_FALSE(SelectionSearchUrl("see (abc.onion)."));
  EXPECT_FALSE(SelectionSearchUrl("text.onion\nmore"));
  EXPECT_TRUE(SelectionSearchUrl("onion soup"));  // The word alone is fine.
}

TEST(ContextMenu, RefusesEmptyAndHugeSelections) {
  EXPECT_FALSE(SelectionSearchUrl(""));
  EXPECT_FALSE(SelectionSearchUrl(" \n\t "));
  EXPECT_FALSE(SelectionSearchUrl(std::string(1025, 'a')));
  EXPECT_TRUE(SelectionSearchUrl(std::string(1024, 'a')));
}

TEST(ContextMenu, ExcerptsAreShortSingleLines) {
  EXPECT_EQ(MenuExcerpt("short", 10), "short");
  EXPECT_EQ(MenuExcerpt("a\n\nb", 10), "a b");
  EXPECT_EQ(MenuExcerpt("abcdefghij", 4), "abcd\xE2\x80\xA6");
  // Cut on a code-point boundary: four 2-byte characters, keep three.
  EXPECT_EQ(MenuExcerpt("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 3),
            "\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x80\xA6");
  EXPECT_EQ(MenuExcerpt("abc", 3), "abc");
}

}  // namespace
}  // namespace torfox::browsing
