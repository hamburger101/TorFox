// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/internal_url.h"

#include "gtest/gtest.h"

namespace torfox {
namespace {

TEST(InternalUrl, ParsesCanonicalUrls) {
  auto a = ParseInternalUrl("torfox://settings/");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->host, "settings");
  EXPECT_EQ(a->path, "/");

  auto b = ParseInternalUrl("torfox://shell/assets/app-1a2b.js?x=1#frag");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->host, "shell");
  EXPECT_EQ(b->path, "/assets/app-1a2b.js");

  auto c = ParseInternalUrl("torfox://newtab");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->path, "/");
}

TEST(InternalUrl, RejectsNonCanonicalOrHostileUrls) {
  for (const char* bad : {
           "", "torfox:", "torfox:settings", "torfox:///settings", "TORFOX://settings/",
           "Torfox://settings/", "torfox://Settings/", "torfox://settings:80/",
           "torfox://user@settings/", "torfox://set%74ings/", "torfox://settings./",
           "torfox://settings\\x", "torfox://settings/a\\b", "torfox://set-tings/",
           "torfox://1/", "torfox://settings/a b", "https://settings/", " torfox://settings/",
           "torfox://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/",  // 33 letters
       }) {
    EXPECT_FALSE(ParseInternalUrl(bad).has_value()) << bad;
  }
}

TEST(InternalUrl, KnownPages) {
  EXPECT_TRUE(IsInternalPage("shell"));
  // Tabs show every page but the window's own and the suggestion list.
  EXPECT_FALSE(IsTabPage("shell"));
  EXPECT_FALSE(IsTabPage("suggest"));
  EXPECT_FALSE(IsTabPage("nope"));
  EXPECT_TRUE(IsTabPage("settings"));
  EXPECT_TRUE(IsTabPage("downloads"));
  EXPECT_TRUE(IsInternalPage("newtab"));
  EXPECT_TRUE(IsInternalPage("settings"));
  EXPECT_FALSE(IsInternalPage("Settings"));
  EXPECT_FALSE(IsInternalPage("evil"));
  EXPECT_FALSE(IsInternalPage(""));
  EXPECT_EQ(InternalPageUrl("settings"), "torfox://settings/");
}

TEST(InternalUrl, MapsOnlyBundledResourcePaths) {
  auto map = [](const char* url) -> std::string {
    auto parsed = ParseInternalUrl(url);
    if (!parsed) {
      return "<unparsed>";
    }
    return ResourcePathFor(*parsed).value_or("<none>");
  };
  EXPECT_EQ(map("torfox://settings/"), "pages/settings/index.html");
  EXPECT_EQ(map("torfox://settings/index.html"), "pages/settings/index.html");
  EXPECT_EQ(map("torfox://shell/?x=1"), "pages/shell/index.html");
  EXPECT_EQ(map("torfox://newtab/assets/newtab-Ab_9.js"), "assets/newtab-Ab_9.js");
  for (const char* none :
       {"torfox://evil/", "torfox://settings/other.html", "torfox://settings/assets/",
        "torfox://settings/assets/../pages/x", "torfox://settings/assets/a/b.js",
        "torfox://settings/assets/.hidden", "torfox://settings/assets/a%2Fb",
        "torfox://settings//assets/x.js", "torfox://settings/pages/settings/index.html",
        "torfox://settings/assets/x..js"}) {
    EXPECT_EQ(map(none), "<none>") << none;
  }
}

TEST(InternalUrl, DetectsInternalSchemeLoosely) {
  for (const char* yes : {"torfox://settings/", "TorFox://x", "  torfox:anything", "tor\tfox://x",
                          "TORFOX:", "torfox\n://x"}) {
    EXPECT_TRUE(HasInternalScheme(yes)) << yes;
  }
  for (const char* no : {"http://torfox/", "torfoxx://x", "torfo://x", "", "about:blank",
                         "javascript:torfox://x"}) {
    EXPECT_FALSE(HasInternalScheme(no)) << no;
  }
}

}  // namespace
}  // namespace torfox
