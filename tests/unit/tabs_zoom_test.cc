// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <cmath>

#include "gtest/gtest.h"
#include "src/core/browsing/tabs.h"
#include "src/core/browsing/zoom.h"

namespace torfox::browsing {
namespace {

TEST(TabStrip, LinksOpenNextToTheirOpener) {
  // Tabs 1..4; 5 and 6 were opened from 2.
  std::vector<TabRef> tabs = {{1, 0}, {2, 0}, {5, 2}, {6, 2}, {3, 0}, {4, 0}};
  EXPECT_EQ(ChildInsertIndex(tabs, 2), 4u);  // After 2's existing children.
  EXPECT_EQ(ChildInsertIndex(tabs, 1), 1u);
  EXPECT_EQ(ChildInsertIndex(tabs, 4), 6u);
  EXPECT_EQ(ChildInsertIndex(tabs, 0), 6u);   // Ctrl+T: the end.
  EXPECT_EQ(ChildInsertIndex(tabs, 99), 6u);  // Opener closed.
  EXPECT_EQ(ChildInsertIndex({}, 1), 0u);
}

TEST(TabStrip, ClosingSelectsTheTabThatTookItsPlace) {
  EXPECT_EQ(NextActiveIndex(1, 3), 1u);
  EXPECT_EQ(NextActiveIndex(3, 3), 2u);  // Closed the last: the new last.
  EXPECT_EQ(NextActiveIndex(0, 0), 0u);
  EXPECT_EQ(ClampIndex(-5, 4), 0u);
  EXPECT_EQ(ClampIndex(2, 4), 2u);
  EXPECT_EQ(ClampIndex(99, 4), 3u);
  EXPECT_EQ(ClampIndex(3, 0), 0u);
}

TEST(TabStrip, ClosedTabsReopenNewestFirstAndAreBounded) {
  ClosedTabs closed;
  closed.Push({"https://a.example/", "A", 0});
  closed.Push({"torfox://newtab/", "New Tab", 1});  // Not worth reopening.
  closed.Push({"", "", 1});
  closed.Push({"https://b.example/", "B", 2});
  ASSERT_EQ(closed.size(), 2u);
  auto b = closed.Pop();
  ASSERT_TRUE(b);
  EXPECT_EQ(b->url, "https://b.example/");
  EXPECT_EQ(b->index, 2u);
  EXPECT_EQ(closed.Pop()->url, "https://a.example/");
  EXPECT_FALSE(closed.Pop());
  for (int i = 0; i < 40; ++i) {
    closed.Push({"https://x.example/" + std::to_string(i), "", 0});
  }
  EXPECT_EQ(closed.size(), ClosedTabs::kMaxEntries);
  EXPECT_EQ(closed.Pop()->url, "https://x.example/39");
}

TEST(Zoom, LevelsMatchChromium) {
  EXPECT_DOUBLE_EQ(ZoomLevelForPercent(100), 0.0);
  EXPECT_NEAR(ZoomLevelForPercent(120), 1.0, 1e-9);
  EXPECT_EQ(ZoomPercentForLevel(0.0), 100);
  for (int step : ZoomSteps()) {
    EXPECT_EQ(ZoomPercentForLevel(ZoomLevelForPercent(step)), step);
  }
}

TEST(Zoom, StepsUpAndDownAndStopsAtTheEnds) {
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(0.0, 1)), 110);
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(0.0, -1)), 90);
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(ZoomLevelForPercent(500), 1)), 500);
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(ZoomLevelForPercent(25), -1)), 25);
  // From an off-step level (e.g. 105%), go to the neighbouring steps.
  const double odd = ZoomLevelForPercent(105);
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(odd, 1)), 110);
  EXPECT_EQ(ZoomPercentForLevel(StepZoomLevel(odd, -1)), 100);
  EXPECT_DOUBLE_EQ(StepZoomLevel(odd, 0), odd);
}

}  // namespace
}  // namespace torfox::browsing
