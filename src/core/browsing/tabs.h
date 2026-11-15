// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_TABS_H_
#define TORFOX_CORE_BROWSING_TABS_H_

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace torfox::browsing {

// Tab-strip rules that do not depend on CEF, so they can be unit tested.

// A tab as the strip rules see it.
struct TabRef {
  int id = 0;
  int opener = 0;  // Tab this one was opened from (a link), or 0.
};

// Where a tab opened from |opener| (a link opened in a new tab) goes: after
// the opener and after the tabs already opened from it, as in other
// browsers. Tabs not opened from a tab (Ctrl+T) go at the end.
std::size_t ChildInsertIndex(const std::vector<TabRef>& tabs, int opener);

// Which tab becomes active when the active tab at |closed_index| closes and
// |remaining| tabs are left: the one that took its place (the next tab),
// or the new last tab.
std::size_t NextActiveIndex(std::size_t closed_index, std::size_t remaining);

// Clamps a requested move target to the strip.
std::size_t ClampIndex(long long requested, std::size_t size);

// Recently closed tabs of one window, newest last. Private windows keep
// theirs only in memory, like everything else about them.
class ClosedTabs {
 public:
  struct Entry {
    std::string url;
    std::string title;
    std::size_t index = 0;  // Where the tab was.
  };
  static constexpr std::size_t kMaxEntries = 25;

  // Records a closed tab. Empty URLs and the new tab page are not worth
  // reopening and are skipped.
  void Push(Entry entry);
  std::optional<Entry> Pop();
  bool empty() const { return entries_.empty(); }
  std::size_t size() const { return entries_.size(); }
  void Clear() { entries_.clear(); }

 private:
  std::deque<Entry> entries_;
};

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_TABS_H_
