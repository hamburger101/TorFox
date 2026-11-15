// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <string>

#include <gtest/gtest.h>

#include "src/core/browsing/session.h"
#include "src/core/json.h"
#include "src/core/tor/onion.h"
#include "src/core/url/nav_input.h"
#include "src/core/url/search.h"

namespace torfox {
namespace {

// ----- Search engines -----------------------------------------------------------

TEST(Search, EachEngineHasItsPage) {
  const std::string q = DuckDuckGoSearchUrl("a b&c");
  EXPECT_EQ(q, "https://duckduckgo.com/?q=a%20b%26c");
  EXPECT_EQ(search::ForEngine(q, "duckduckgo", false), q);
  EXPECT_EQ(search::ForEngine(q, "duckduckgo-html", false),
            "https://html.duckduckgo.com/html/?q=a%20b%26c");
  EXPECT_EQ(search::ForEngine(q, "duckduckgo-lite", false),
            "https://lite.duckduckgo.com/lite/?q=a%20b%26c");
  // Unknown or missing: the default page, never anything else.
  EXPECT_EQ(search::ForEngine(q, "", false), q);
  EXPECT_EQ(search::ForEngine(q, "google", false), q);
}

TEST(Search, TorWindowsUseDuckDuckGosOnionService) {
  const std::string q = DuckDuckGoSearchUrl("tor words");
  const std::string onion = "https://" + std::string(tor::kDuckDuckGoOnion);
  EXPECT_EQ(search::ForEngine(q, "duckduckgo", true), onion + "/?q=tor%20words");
  EXPECT_EQ(search::ForEngine(q, "duckduckgo-html", true), onion + "/html/?q=tor%20words");
  EXPECT_EQ(search::ForEngine(q, "duckduckgo-lite", true), onion + "/lite/?q=tor%20words");
  EXPECT_EQ(search::ForEngine(q, "nonsense", true), onion + "/?q=tor%20words");
}

TEST(Search, OtherUrlsAreLeftAlone) {
  for (const char* url : {"https://example.com/?q=a", "https://duckduckgo.com.evil/?q=a",
                          "http://duckduckgo.com/?q=a", "https://duckduckgo.com/about"}) {
    EXPECT_EQ(search::ForEngine(url, "duckduckgo-lite", true), url) << url;
    EXPECT_EQ(search::ForEngine(url, "duckduckgo-html", false), url) << url;
  }
}

// ----- Sessions -----------------------------------------------------------------

using browsing::ParseSession;
using browsing::RestorableUrl;
using browsing::SerializeSession;
using browsing::Session;

TEST(Session, KeepsWebPagesAndTorFoxPagesOnly) {
  EXPECT_EQ(RestorableUrl("https://example.com/a?b#c"), "https://example.com/a?b#c");
  EXPECT_EQ(RestorableUrl("http://127.0.0.1:8080/x"), "http://127.0.0.1:8080/x");
  EXPECT_EQ(RestorableUrl("torfox://settings/"), "torfox://settings/");
  EXPECT_EQ(RestorableUrl("torfox://history/?q=x"), "torfox://history/");
  EXPECT_EQ(RestorableUrl("torfox://newtab/"), "torfox://newtab/");
  const std::string onion = std::string(tor::kDuckDuckGoOnion);
  for (const std::string& bad :
       {std::string("torfox://shell/"), std::string("torfox://suggest/"), std::string("torfox://nosuchpage/"), std::string("TORFOX://settings/"),
        std::string("javascript:alert(1)"), std::string("data:text/html,x"), std::string("file:///C:/x"),
        std::string("about:blank"), std::string(""), "http://" + onion + "/", "https://" + onion + "/html/",
        "https://sub." + onion + "/", std::string("https://exa mple.com/")}) {
    EXPECT_FALSE(RestorableUrl(bad).has_value()) << bad;
  }
}

TEST(Session, RoundTrips) {
  Session s = {{{"https://a.example/", "torfox://settings/", "https://b.example/x"}, 2},
               {{"https://c.example/"}, 0}};
  const Session back = ParseSession(SerializeSession(s));
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[0].urls, s[0].urls);
  EXPECT_EQ(back[0].active, 2u);
  EXPECT_EQ(back[1].urls, s[1].urls);
  EXPECT_EQ(back[1].active, 0u);
}

TEST(Session, UnrestorableTabsAndEmptyWindowsAreLeftOut) {
  const std::string onion = "http://" + std::string(tor::kDuckDuckGoOnion) + "/";
  // The active tab moves with the tabs kept before it.
  Session s = {{{"javascript:x", "https://a.example/", onion, "https://b.example/"}, 3},
               {{onion, "torfox://shell/"}, 0},
               {{"https://c.example/", onion}, 1}};
  const Json saved = *ParseJson(SerializeSession(s), 1 << 20, 8);
  EXPECT_EQ(saved, Json::parse(R"({"format":1,"windows":[
      {"tabs":["https://a.example/","https://b.example/"],"active":1},
      {"tabs":["https://c.example/"],"active":0}]})"));
  EXPECT_EQ(SerializeSession(s).find("onion"), std::string::npos);
}

TEST(Session, DamagedOrForeignFilesRestoreNothingUnsafe) {
  for (const char* text : {"", "{", "[]", "null", R"({"windows":[{"tabs":["https://a.example/"]}]})",
                           R"({"format":2,"windows":[{"tabs":["https://a.example/"]}]})",
                           R"({"format":"1","windows":[{"tabs":["https://a.example/"]}]})",
                           R"({"format":1,"windows":{}})", R"({"format":1,"windows":[{"tabs":"x"}]})",
                           R"({"format":1,"windows":[{"tabs":[1,true,null,{}]}]})",
                           R"j({"format":1,"windows":[{"tabs":["javascript:alert(1)","torfox://shell/"]}]})j"}) {
    EXPECT_TRUE(ParseSession(text).empty()) << text;
  }
  // Bad parts of a file are dropped, good ones kept; |active| is clamped.
  const Session s = ParseSession(R"({"format":1,"windows":[
      {"tabs":["https://a.example/", 7, "file:///C:/x", "https://b.example/"],"active":99},
      "not a window",
      {"tabs":["https://c.example/"],"active":-1}]})");
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0].urls, (std::vector<std::string>{"https://a.example/", "https://b.example/"}));
  EXPECT_EQ(s[0].active, 1u);
  EXPECT_EQ(s[1].active, 0u);
}

TEST(Session, LimitsAreKept) {
  Session big;
  for (std::size_t w = 0; w < browsing::kMaxSessionWindows + 5; ++w) {
    browsing::SessionWindow win;
    for (std::size_t t = 0; t < browsing::kMaxSessionTabs + 5; ++t) {
      win.urls.push_back("https://a.example/" + std::to_string(t));
    }
    big.push_back(win);
  }
  const Session back = ParseSession(SerializeSession(big));
  ASSERT_EQ(back.size(), browsing::kMaxSessionWindows);
  for (const auto& w : back) {
    EXPECT_EQ(w.urls.size(), browsing::kMaxSessionTabs);
  }
  // A file over the size limit is ignored as a whole.
  std::string huge = R"({"format":1,"windows":[{"tabs":["https://a.example/"]}],"pad":")";
  huge += std::string(browsing::kMaxSessionBytes, 'x');
  huge += "\"}";
  EXPECT_TRUE(ParseSession(huge).empty());
}

}  // namespace
}  // namespace torfox
