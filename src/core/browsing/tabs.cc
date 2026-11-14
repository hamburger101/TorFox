// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/tabs.h"

#include <utility>

#include "src/core/url/internal_url.h"

namespace torfox::browsing {

std::size_t ChildInsertIndex(const std::vector<TabRef>& tabs, int opener) {
  if (opener == 0) {
    return tabs.size();
  }
  std::size_t i = 0;
  while (i < tabs.size() && tabs[i].id != opener) {
    ++i;
  }
  if (i == tabs.size()) {
    return tabs.size();  // The opener is gone.
  }
  ++i;
  while (i < tabs.size() && tabs[i].opener == opener) {
    ++i;
  }
  return i;
}

std::size_t NextActiveIndex(std::size_t closed_index, std::size_t remaining) {
  if (remaining == 0) {
    return 0;
  }
  return closed_index < remaining ? closed_index : remaining - 1;
}

std::size_t ClampIndex(long long requested, std::size_t size) {
  if (size == 0 || requested <= 0) {
    return 0;
  }
  const auto last = static_cast<long long>(size - 1);
  return static_cast<std::size_t>(requested > last ? last : requested);
}

void ClosedTabs::Push(Entry entry) {
  if (entry.url.empty() || entry.url == InternalPageUrl("newtab")) {
    return;
  }
  entries_.push_back(std::move(entry));
  while (entries_.size() > kMaxEntries) {
    entries_.pop_front();
  }
}

std::optional<ClosedTabs::Entry> ClosedTabs::Pop() {
  if (entries_.empty()) {
    return std::nullopt;
  }
  Entry e = std::move(entries_.back());
  entries_.pop_back();
  return e;
}

}  // namespace torfox::browsing
