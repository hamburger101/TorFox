// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Settings > Memory: how TorFox's processes are grouped and named.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/core/memory/report.h"

namespace torfox::memory {
namespace {

using Kind = Process::Kind;

Renderer Tab(long pid, int64_t key, std::string title, std::string window = "normal",
             bool active = false) {
  Renderer r;
  r.pid = pid;
  r.tab = true;
  r.tab_key = key;
  r.title = std::move(title);
  r.window = std::move(window);
  r.active = active;
  return r;
}

Renderer Interface(long pid) {
  Renderer r;
  r.pid = pid;
  r.window = "normal";
  return r;
}

TEST(MemoryReport, GroupsProcessesAsTheUserKnowsThem) {
  const std::vector<Process> processes = {
      {1, Kind::kBrowser, 100}, {2, Kind::kHelper, 40},  // GPU: nobody's renderer
      {3, Kind::kHelper, 30},   {4, Kind::kHelper, 20},  // tab A; tab B
      {5, Kind::kHelper, 10},   {6, Kind::kTor, 7},      // toolbar; tor
      {7, Kind::kTor, 3},                                // a pluggable transport
  };
  const std::vector<Renderer> renderers = {Tab(3, 11, "A", "normal", true), Tab(4, 12, "B"),
                                           Interface(5)};
  const Json r = BuildReport(processes, renderers);
  EXPECT_TRUE(r["supported"].get<bool>());
  EXPECT_EQ(r["total"].get<uint64_t>(), 210u);
  EXPECT_EQ(r["processes"].get<int>(), 7);
  EXPECT_EQ(r["browser"].get<uint64_t>(), 100u);
  EXPECT_EQ(r["helpers"]["bytes"].get<uint64_t>(), 40u);
  EXPECT_EQ(r["helpers"]["pids"], Json::array({2}));
  EXPECT_EQ(r["interface"]["bytes"].get<uint64_t>(), 10u);
  EXPECT_EQ(r["tor"]["bytes"].get<uint64_t>(), 10u);
  EXPECT_EQ(r["tor"]["processes"].get<int>(), 2);
  ASSERT_EQ(r["tabs"].size(), 2u);
  // Largest first.
  EXPECT_EQ(r["tabs"][0]["tabs"][0]["title"], "A");
  EXPECT_TRUE(r["tabs"][0]["tabs"][0]["active"].get<bool>());
  EXPECT_EQ(r["tabs"][0]["bytes"].get<uint64_t>(), 30u);
  EXPECT_EQ(r["tabs"][1]["tabs"][0]["title"], "B");
  EXPECT_EQ(r["tabs"][1]["pids"], Json::array({4}));
}

TEST(MemoryReport, SharedProcessesAndCrossSiteFrames) {
  const std::vector<Process> processes = {
      {1, Kind::kBrowser, 1}, {3, Kind::kHelper, 30}, {4, Kind::kHelper, 20}, {5, Kind::kHelper, 5}};
  // Tabs 11 and 12 share process 3; tab 13's page is in 4 and a frame of it in 5.
  const std::vector<Renderer> renderers = {Tab(3, 11, "A"), Tab(3, 12, "B"), Tab(4, 13, "C"),
                                           Tab(5, 13, "C")};
  const Json r = BuildReport(processes, renderers);
  ASSERT_EQ(r["tabs"].size(), 2u);
  EXPECT_EQ(r["tabs"][0]["tabs"].size(), 2u);
  EXPECT_EQ(r["tabs"][0]["processes"].get<int>(), 1);
  EXPECT_EQ(r["tabs"][1]["tabs"].size(), 1u);
  EXPECT_EQ(r["tabs"][1]["tabs"][0]["title"], "C");
  EXPECT_EQ(r["tabs"][1]["processes"].get<int>(), 2);
  EXPECT_EQ(r["tabs"][1]["bytes"].get<uint64_t>(), 25u);
}

TEST(MemoryReport, PrivateAndTorTabsAreNotNamed) {
  const std::vector<Process> processes = {
      {1, Kind::kBrowser, 1}, {3, Kind::kHelper, 3}, {4, Kind::kHelper, 2}, {5, Kind::kHelper, 1}};
  const std::vector<Renderer> renderers = {Tab(3, 11, "Secret page", "private"),
                                           Tab(4, 12, "Onion page", "tor"),
                                           Tab(5, 13, "Odd", "something else")};
  const Json r = BuildReport(processes, renderers);
  ASSERT_EQ(r["tabs"].size(), 3u);
  EXPECT_EQ(r["tabs"][0]["tabs"][0]["title"], "");
  EXPECT_EQ(r["tabs"][0]["tabs"][0]["window"], "private");
  EXPECT_EQ(r["tabs"][1]["tabs"][0]["title"], "");
  EXPECT_EQ(r["tabs"][1]["tabs"][0]["window"], "tor");
  // An unknown kind of window is treated as private.
  EXPECT_EQ(r["tabs"][2]["tabs"][0]["title"], "");
  EXPECT_EQ(r["tabs"][2]["tabs"][0]["window"], "private");
  EXPECT_EQ(r.dump().find("Secret"), std::string::npos);
  EXPECT_EQ(r.dump().find("Onion"), std::string::npos);
}

TEST(MemoryReport, WhatARendererSaysOnlyAttributesTorFoxsOwnHelpers) {
  // A renderer may name any process: one that is not a TorFox helper (here
  // the browser, tor, or a process that is not TorFox's at all) is not
  // counted for its tab.
  const std::vector<Process> processes = {
      {1, Kind::kBrowser, 100}, {6, Kind::kTor, 7}, {3, Kind::kHelper, 30}};
  const std::vector<Renderer> renderers = {Tab(1, 11, "Liar"), Tab(6, 12, "Liar"),
                                           Tab(999, 13, "Liar"), Tab(-5, 14, "Liar"),
                                           Tab(3, 15, "Honest")};
  const Json r = BuildReport(processes, renderers);
  EXPECT_EQ(r["total"].get<uint64_t>(), 137u);
  EXPECT_EQ(r["browser"].get<uint64_t>(), 100u);
  EXPECT_EQ(r["tor"]["bytes"].get<uint64_t>(), 7u);
  ASSERT_EQ(r["tabs"].size(), 1u);
  EXPECT_EQ(r["tabs"][0]["tabs"][0]["title"], "Honest");
}

TEST(MemoryReport, EachProcessCountsOnce) {
  const std::vector<Process> processes = {
      {1, Kind::kBrowser, 10}, {3, Kind::kHelper, 5}, {3, Kind::kHelper, 5}, {0, Kind::kHelper, 9}};
  const Json r = BuildReport(processes, {Tab(3, 11, "A"), Tab(3, 11, "A")});
  EXPECT_EQ(r["total"].get<uint64_t>(), 15u);
  EXPECT_EQ(r["processes"].get<int>(), 2);
  ASSERT_EQ(r["tabs"].size(), 1u);
  EXPECT_EQ(r["tabs"][0]["tabs"].size(), 1u);
}

TEST(MemoryReport, LongAndInvalidTitles) {
  const std::string long_title = std::string(kMaxTitleBytes - 1, 'a') + "\xC3\xA9" + "tail";
  const std::vector<Process> processes = {{1, Kind::kBrowser, 1}, {3, Kind::kHelper, 2},
                                          {4, Kind::kHelper, 1}};
  const Json r = BuildReport(processes, {Tab(3, 11, long_title), Tab(4, 12, "bad \xC3\x28")});
  const std::string shown = r["tabs"][0]["tabs"][0]["title"].get<std::string>();
  // Cut before the two-byte character, not inside it, then an ellipsis.
  EXPECT_EQ(shown, std::string(kMaxTitleBytes - 1, 'a') + "\xE2\x80\xA6");
  EXPECT_EQ(r["tabs"][1]["tabs"][0]["title"], "");
}

TEST(MemoryReport, NothingMeasured) {
  const Json r = BuildReport({}, {Tab(3, 11, "A")});
  EXPECT_FALSE(r["supported"].get<bool>());
  EXPECT_EQ(r["total"].get<uint64_t>(), 0u);
  EXPECT_EQ(r["tabs"].size(), 0u);
}

}  // namespace
}  // namespace torfox::memory
