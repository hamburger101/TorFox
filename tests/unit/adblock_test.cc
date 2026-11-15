// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/core/adblock/filters.h"
#include "src/core/adblock/sites.h"

namespace torfox::adblock {
namespace {

using Rules = std::vector<std::string_view>;

bool Blocks(const Engine& e, std::string_view url, std::string_view page = "https://page.test/",
            Type type = kScript, std::string_view frame = {}) {
  Request r;
  r.url = url;
  r.top_url = page;
  r.frame_url = frame;
  r.type = type;
  return e.Check(r).block;
}

TEST(AdBlock, HostRulesMatchTheHostAndItsSubdomainsOnly) {
  const Rules rules = {"||ads.example.com^"};
  const Engine e(rules);
  EXPECT_EQ(e.request_rules(), 1u);
  EXPECT_TRUE(Blocks(e, "https://ads.example.com/x.js"));
  EXPECT_TRUE(Blocks(e, "https://ads.example.com"));
  EXPECT_TRUE(Blocks(e, "http://ads.example.com:8080/x"));
  EXPECT_TRUE(Blocks(e, "https://cdn.ads.example.com/x.js"));
  EXPECT_TRUE(Blocks(e, "https://ADS.Example.COM/x.js"));
  EXPECT_TRUE(Blocks(e, "wss://ads.example.com/socket", "https://page.test/", kWebSocket));
  EXPECT_FALSE(Blocks(e, "https://notads.example.com/x.js"));
  EXPECT_FALSE(Blocks(e, "https://ads.example.com.evil.test/x.js"));
  EXPECT_FALSE(Blocks(e, "https://example.com/ads.example.com/"));
  EXPECT_FALSE(Blocks(e, "https://user:ads.example.com@other.test/"));
}

TEST(AdBlock, ATrailingDotDoesNotHideAHost) {
  const Rules rules = {"||ads.example.com^", "||track.test/pixel"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://ads.example.com./x.js"));
  EXPECT_TRUE(Blocks(e, "https://track.test./pixel?a=1"));
}

TEST(AdBlock, AnchorsWildcardsAndSeparators) {
  const Rules rules = {"|https://start.test/ad", "swf|", "/banner/*/img^", "&adtype=pop^"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://start.test/ad.js"));
  EXPECT_FALSE(Blocks(e, "https://other.test/?u=https://start.test/ad"));
  EXPECT_TRUE(Blocks(e, "https://x.test/movie.swf"));
  EXPECT_FALSE(Blocks(e, "https://x.test/movie.swf?x=1"));
  EXPECT_TRUE(Blocks(e, "https://x.test/banner/a/b/img?x=1"));
  EXPECT_TRUE(Blocks(e, "https://x.test/banner/a/img"));
  EXPECT_FALSE(Blocks(e, "https://x.test/banner/a/imgs"));
  EXPECT_FALSE(Blocks(e, "https://x.test/banners/img"));
  EXPECT_TRUE(Blocks(e, "https://x.test/p?id=1&adtype=pop&z=2"));
  EXPECT_TRUE(Blocks(e, "https://x.test/p?id=1&adtype=pop"));
  EXPECT_FALSE(Blocks(e, "https://x.test/p?id=1&adtype=popup"));
}

TEST(AdBlock, ExceptionsLetRequestsThroughAndSayWhich) {
  const Rules rules = {"||ads.example.com^", "@@||ads.example.com/allowed/"};
  const Engine e(rules);
  Request r;
  r.url = "https://ads.example.com/allowed/x.js";
  r.top_url = "https://page.test/";
  r.type = kScript;
  const Verdict v = e.Check(r);
  EXPECT_FALSE(v.block);
  EXPECT_EQ(v.rule, "@@||ads.example.com/allowed/");
  r.url = "https://ads.example.com/other/x.js";
  EXPECT_EQ(e.Check(r).rule, "||ads.example.com^");
  EXPECT_TRUE(e.Check(r).block);
}

TEST(AdBlock, ImportantRulesIgnoreExceptions) {
  const Rules rules = {"||ads.example.com^$important", "@@||ads.example.com^"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://ads.example.com/x.js"));
  const Rules both = {"||ads.example.com^", "||ads.example.com/x$important", "@@||ads.example.com^"};
  const Engine e2(both);
  EXPECT_TRUE(Blocks(e2, "https://ads.example.com/x.js"));
  EXPECT_FALSE(Blocks(e2, "https://ads.example.com/y.js"));
}

TEST(AdBlock, TypeOptions) {
  const Rules rules = {"||s.test^$script", "||n.test^$~image", "||p.test^$popup",
                       "||f.test^$subdocument,xmlhttprequest", "||all.test^"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://s.test/a", "https://page.test/", kScript));
  EXPECT_FALSE(Blocks(e, "https://s.test/a", "https://page.test/", kImage));
  EXPECT_TRUE(Blocks(e, "https://n.test/a", "https://page.test/", kScript));
  EXPECT_FALSE(Blocks(e, "https://n.test/a", "https://page.test/", kImage));
  EXPECT_FALSE(Blocks(e, "https://n.test/a", "https://page.test/", kPopup));
  EXPECT_TRUE(Blocks(e, "https://p.test/a", "https://page.test/", kPopup));
  EXPECT_FALSE(Blocks(e, "https://p.test/a", "https://page.test/", kScript));
  EXPECT_TRUE(Blocks(e, "https://f.test/a", "https://page.test/", kSubdocument));
  EXPECT_TRUE(Blocks(e, "https://f.test/a", "https://page.test/", kXmlHttpRequest));
  EXPECT_FALSE(Blocks(e, "https://f.test/a", "https://page.test/", kImage));
  // A rule naming no type applies to every request, but not to popups.
  EXPECT_TRUE(Blocks(e, "https://all.test/a", "https://page.test/", kMedia));
  EXPECT_TRUE(Blocks(e, "https://all.test/a", "https://page.test/", kOther));
  EXPECT_FALSE(Blocks(e, "https://all.test/a", "https://page.test/", kPopup));
}

TEST(AdBlock, ThirdPartyAndFirstParty) {
  const Rules rules = {"||cdn.test^$third-party", "||self.test/ad^$~third-party"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://cdn.test/a.js", "https://news.example/"));
  EXPECT_FALSE(Blocks(e, "https://cdn.test/a.js", "https://cdn.test/"));
  EXPECT_FALSE(Blocks(e, "https://img.cdn.test/a.js", "https://www.cdn.test/"));
  EXPECT_TRUE(Blocks(e, "https://self.test/ad/x", "https://www.self.test/"));
  EXPECT_FALSE(Blocks(e, "https://self.test/ad/x", "https://news.example/"));
  // No document known: counted as third-party.
  EXPECT_TRUE(Blocks(e, "https://cdn.test/a.js", ""));
}

TEST(AdBlock, TheFramesDocumentDecidesDomainAndParty) {
  const Rules rules = {"||ads.test^$domain=news.example", "||cdn.test^$third-party"};
  const Engine e(rules);
  // A request from an iframe on news.example: its own document is elsewhere.
  EXPECT_FALSE(Blocks(e, "https://ads.test/a", "https://news.example/", kScript,
                      "https://widget.test/frame"));
  EXPECT_TRUE(Blocks(e, "https://ads.test/a", "https://news.example/", kScript,
                     "https://news.example/frame"));
  EXPECT_FALSE(Blocks(e, "https://cdn.test/a", "https://news.example/", kScript,
                      "https://www.cdn.test/frame"));
}

TEST(AdBlock, DomainOption) {
  const Rules rules = {"||x.test^$domain=a.example|~b.a.example"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://x.test/", "https://a.example/"));
  EXPECT_TRUE(Blocks(e, "https://x.test/", "https://c.a.example/"));
  EXPECT_FALSE(Blocks(e, "https://x.test/", "https://b.a.example/"));
  EXPECT_FALSE(Blocks(e, "https://x.test/", "https://d.b.a.example/"));
  EXPECT_FALSE(Blocks(e, "https://x.test/", "https://other.example/"));
  EXPECT_FALSE(Blocks(e, "https://x.test/", ""));
  // The most specific entry decides.
  const Rules nested = {"||y.test^$domain=~a.example|b.a.example"};
  const Engine e2(nested);
  EXPECT_TRUE(Blocks(e2, "https://y.test/", "https://b.a.example/"));
  EXPECT_FALSE(Blocks(e2, "https://y.test/", "https://a.example/"));
  EXPECT_FALSE(Blocks(e2, "https://y.test/", "https://other.example/"));
}

TEST(AdBlock, MatchCase) {
  const Rules rules = {"/BannerAd.$match-case", "/popunder."};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://x.test/BannerAd.js"));
  EXPECT_FALSE(Blocks(e, "https://x.test/bannerad.js"));
  EXPECT_TRUE(Blocks(e, "https://x.test/PopUnder.js"));
}

TEST(AdBlock, DocumentExceptionsTurnBlockingOff) {
  const Rules rules = {"||ads.test^", "||trusted.example/ad.js", "@@||trusted.example^$document"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://ads.test/a", "https://news.example/"));
  EXPECT_FALSE(Blocks(e, "https://ads.test/a", "https://trusted.example/page"));
  EXPECT_FALSE(Blocks(e, "https://ads.test/a", "https://www.trusted.example/"));
  // In a frame of a trusted page, too.
  EXPECT_FALSE(Blocks(e, "https://ads.test/a", "https://trusted.example/", kScript,
                      "https://widget.test/"));
  EXPECT_TRUE(e.ForPage("https://trusted.example/").disabled);
  EXPECT_FALSE(e.ForPage("https://news.example/").disabled);
  // An exception about documents is not an exception for requests.
  EXPECT_TRUE(Blocks(e, "https://trusted.example/ad.js", "https://news.example/"));
}

TEST(AdBlock, PageOptions) {
  const Rules rules = {"@@||a.example^$generichide", "@@||b.example^$elemhide",
                       "@@||c.example^$genericblock", "||ads.test^", "||ads.test^$domain=c.example"};
  const Engine e(rules);
  EXPECT_TRUE(e.ForPage("https://a.example/").no_generic_hiding);
  EXPECT_FALSE(e.ForPage("https://a.example/").no_hiding);
  EXPECT_TRUE(e.ForPage("https://b.example/x").no_hiding);
  EXPECT_TRUE(e.ForPage("https://c.example/").no_generic_blocking);
  EXPECT_FALSE(e.ForPage("https://d.example/").no_generic_hiding);
  // genericblock: rules for every site are skipped, rules for the site still apply.
  const Rules generic_only = {"@@||c.example^$genericblock", "||ads.test^"};
  const Engine e2(generic_only);
  EXPECT_FALSE(Blocks(e2, "https://ads.test/a", "https://c.example/"));
  EXPECT_TRUE(Blocks(e, "https://ads.test/a", "https://c.example/"));
}

TEST(AdBlock, RulesNotUnderstoodAreNeverApplied) {
  const Rules rules = {
      "/ads[0-9]+/",                  // regular expression
      "||csp.test^$csp=script-src 'none'",
      "||redir.test^$redirect=noop.js",
      "||doc.test^$document",          // would block a page the user opens
      "||ehide.test^$elemhide",        // only meaningful as an exception
      "$popup,third-party",            // every popup everywhere: too broad
      "*$script",                      // every script everywhere
      "||",                            // nothing
      "||opt.test^$unknownoption",
      "||dom.test^$domain=",
      "||ent.test^$domain=site.*|other.example|",
      "! a comment",
      "[Adblock Plus 2.0]",
      "",
      "||ok.test^",
  };
  const Engine e(rules);
  EXPECT_EQ(e.request_rules(), 1u);
  EXPECT_EQ(e.ignored_rules(), 11u);
  EXPECT_TRUE(Blocks(e, "https://ok.test/"));
  for (std::string_view url : {"https://x.test/ads12/", "https://csp.test/", "https://redir.test/",
                               "https://doc.test/", "https://ehide.test/", "https://opt.test/",
                               "https://dom.test/", "https://ent.test/"}) {
    EXPECT_FALSE(Blocks(e, url)) << url;
  }
  EXPECT_FALSE(Blocks(e, "https://pop.test/", "https://page.test/", kPopup));
}

TEST(AdBlock, PopupRulesLimitedToSites) {
  const Rules rules = {"$popup,third-party,domain=video.example", "||popads.test^$popup"};
  const Engine e(rules);
  EXPECT_TRUE(Blocks(e, "https://anything.test/x", "https://video.example/", kPopup));
  EXPECT_FALSE(Blocks(e, "https://www.video.example/next", "https://video.example/", kPopup));
  EXPECT_FALSE(Blocks(e, "https://anything.test/x", "https://other.example/", kPopup));
  EXPECT_TRUE(Blocks(e, "https://popads.test/go", "https://other.example/", kPopup));
}

TEST(AdBlock, ElementHiding) {
  const Rules rules = {"##.ad-box", "###sponsor", "example.com##.promo", "example.com#@#.ad-box",
                       "shop.example.com,~deals.shop.example.com##.banner", "~example.com##.gone"};
  const Engine e(rules);
  EXPECT_EQ(e.hiding_rules(), 6u);
  const std::string other = e.HidingCss("other.test", true);
  EXPECT_NE(other.find(".ad-box{display:none!important}\n"), std::string::npos);
  EXPECT_NE(other.find("#sponsor{display:none!important}\n"), std::string::npos);
  EXPECT_NE(other.find(".gone{"), std::string::npos);
  EXPECT_EQ(other.find(".promo"), std::string::npos);
  const std::string site = e.HidingCss("www.example.com", true);
  EXPECT_NE(site.find(".promo{"), std::string::npos);
  EXPECT_EQ(site.find(".ad-box"), std::string::npos);  // Excepted on example.com.
  EXPECT_NE(site.find("#sponsor{"), std::string::npos);
  EXPECT_EQ(site.find(".gone"), std::string::npos);
  // Site-specific rules only (frames: TorFox adds generic rules to top-level documents only).
  EXPECT_EQ(e.HidingCss("example.com", false), ".promo{display:none!important}\n");
  EXPECT_EQ(e.HidingCss("other.test", false), "");
  EXPECT_NE(e.HidingCss("a.shop.example.com", false).find(".banner{"), std::string::npos);
  EXPECT_EQ(e.HidingCss("deals.shop.example.com", false).find(".banner"), std::string::npos);
  EXPECT_EQ(e.HidingCss("EXAMPLE.com.", false), ".promo{display:none!important}\n");
}

TEST(AdBlock, EntityDomains) {
  const Rules rules = {"example.*##.x", "www.example.*##.y"};
  const Engine e(rules);
  EXPECT_NE(e.HidingCss("example.com", false).find(".x{"), std::string::npos);
  EXPECT_NE(e.HidingCss("www.example.co.uk", false).find(".x{"), std::string::npos);
  EXPECT_NE(e.HidingCss("www.example.co.uk", false).find(".y{"), std::string::npos);
  EXPECT_EQ(e.HidingCss("example.co.uk", false).find(".y"), std::string::npos);
  EXPECT_EQ(e.HidingCss("notexample.com", false), "");
  EXPECT_EQ(e.HidingCss("example.com.evil.test", false), "");
}

TEST(AdBlock, SelectorsThatCouldEscapeTheirRuleAreIgnored) {
  for (std::string_view ok : {".ad", "#a > .b", "div[class^=\"ad-\"]", "a[href*='/*']",
                              "div:not(.x):has(> .ad)", ".a\\:b", "[data-x=\"{}\"]",
                              "a[href*=\"ad;\"]"}) {
    EXPECT_TRUE(SafeSelector(ok)) << ok;
  }
  for (std::string_view bad :
       {"", ".a{background:url(https://x.test/)}", ".a}body{display:none", ".a;", "/* x",
        ".a[x=\"y", ".a:not(.b", ".a)", ".a]", "@import url(x)", ".a\\", ".a\n.b", ".a !important",
        "<style>", ".a */"}) {
    EXPECT_FALSE(SafeSelector(bad)) << bad;
  }
  const Rules rules = {"##.a{background:url(https://x.test/)}", "example.com##.b}body{x", "##.ok"};
  const Engine e(rules);
  EXPECT_EQ(e.hiding_rules(), 1u);
  EXPECT_EQ(e.ignored_rules(), 2u);
  EXPECT_EQ(e.HidingCss("example.com", true), ".ok{display:none!important}\n");
}

TEST(AdBlock, UnsupportedHidingSyntaxIsIgnored) {
  const Rules rules = {"example.com#?#.a:-abp-has(.b)", "example.com##+js(nowebrtc)",
                       "example.com#$#abort-on-property-read x", "example.com#@?#.c",
                       "example.com#%#window.x=1", "bad domain##.d"};
  const Engine e(rules);
  EXPECT_EQ(e.hiding_rules(), 0u);
  EXPECT_EQ(e.ignored_rules(), 6u);
  EXPECT_EQ(e.HidingCss("example.com", true), "");
}

TEST(AdBlock, TheBuiltInListLoads) {
  const auto start = std::chrono::steady_clock::now();
  const Engine& e = BuiltIn();
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  EXPECT_EQ(BuiltInCommit().size(), 40u);
  EXPECT_GT(BuiltInRuleCount(), 70000u);
  EXPECT_EQ(e.request_rules() + e.hiding_rules() + e.ignored_rules(), BuiltInRuleCount());
  EXPECT_GT(e.request_rules(), 50000u);
  EXPECT_GT(e.hiding_rules(), 20000u);
  // Some rules are not usable (unsafe selectors, $document blocks, ...), but few.
  EXPECT_LT(e.ignored_rules(), BuiltInRuleCount() / 50);
  std::printf("[ built-in list: %zu rules, %zu request, %zu hiding, %zu ignored; %.0f ms, ~%zu KB ]\n",
              BuiltInRuleCount(), e.request_rules(), e.hiding_rules(), e.ignored_rules(), ms,
              e.memory_bytes() / 1024);
  // Real rules from the pinned list.
  EXPECT_TRUE(Blocks(e, "https://ib.adnxs.com/ut/v3", "https://news.example/", kXmlHttpRequest));
  EXPECT_TRUE(Blocks(e, "https://c.amazon-adsystem.com/aax2/apstag.js", "https://news.example/"));
  EXPECT_TRUE(Blocks(e, "https://popads.net/x", "https://news.example/", kPopup));
  EXPECT_FALSE(Blocks(e, "https://news.example/app.js", "https://news.example/"));
  EXPECT_FALSE(Blocks(e, "https://cdn.jsdelivr.net/npm/lit@3/index.js", "https://news.example/"));
  EXPECT_TRUE(e.ForPage("https://accounts.google.com/signin").no_generic_hiding);
  const std::string css = e.HidingCss("news.example", true);
  EXPECT_NE(css.find("\n.ad--banner{display:none!important}\n"), std::string::npos);
  EXPECT_NE(e.HidingCss("1001games.com", false).find(".gc-leaderboard{"), std::string::npos);
}

TEST(AdBlock, CheckingIsFast) {
  const Engine& e = BuiltIn();
  const std::vector<std::string> urls = {
      "https://www.news.example/assets/app.3f2a.js",
      "https://fonts.example/css2?family=Inter:wght@400;700&display=swap",
      "https://img.news.example/2026/11/15/photo-of-the-day.jpg?w=1200&q=80",
      "https://api.news.example/v2/comments?article=12345&page=2",
      "https://ib.adnxs.com/ut/v3",
      "https://c.amazon-adsystem.com/aax2/apstag.js",
      "https://cdn.example/libs/react/18.3.1/react.production.min.js",
      "https://www.news.example/static/banner/promo/summer.png",
  };
  constexpr int kRounds = 2000;
  int blocked = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kRounds; ++i) {
    for (const std::string& url : urls) {
      blocked += Blocks(e, url, "https://www.news.example/world/story.html", kScript) ? 1 : 0;
    }
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                        .count() /
                    (kRounds * urls.size());
  std::printf("[ %.1f us per request ]\n", us);
  EXPECT_GE(blocked, kRounds * 2);
  EXPECT_LT(us, 200.0);  // Generous: debug builds and slow CI machines.
  const auto css_start = std::chrono::steady_clock::now();
  const std::string css = e.HidingCss("www.news.example", true);
  const double css_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - css_start).count();
  std::printf("[ element hiding: %zu KB in %.2f ms ]\n", css.size() / 1024, css_ms);
  EXPECT_LT(css_ms, 100.0);
}

TEST(AdBlockSites, TheSettingHoldsHostNames) {
  EXPECT_TRUE(SitesTextIsValid(""));
  EXPECT_TRUE(SitesTextIsValid("example.com\nNews.Example.org.\n\n  shop.test  \n192.168.1.1\n"));
  EXPECT_FALSE(SitesTextIsValid("example.com\nhttps://x.test/\n"));
  EXPECT_FALSE(SitesTextIsValid("a b.test"));
  EXPECT_FALSE(SitesTextIsValid("-bad.test"));
  EXPECT_FALSE(SitesTextIsValid("bad-.test"));
  EXPECT_FALSE(SitesTextIsValid("a..test"));
  EXPECT_FALSE(SitesTextIsValid("*.test"));
  std::string many;
  for (int i = 0; i < 1001; ++i) {
    many += "s" + std::to_string(i) + ".test\n";
  }
  EXPECT_FALSE(SitesTextIsValid(many));
  EXPECT_EQ(ParseSites("Example.COM.\nexample.com\nbad site\nshop.test"),
            (std::vector<std::string>{"example.com", "shop.test"}));
  EXPECT_EQ(SitesText({"a.test", "b.test"}), "a.test\nb.test\n");
}

TEST(AdBlockSites, AnEntryCoversItsSubdomains) {
  const std::vector<std::string> sites = {"example.com"};
  EXPECT_EQ(CoveringSite(sites, "example.com"), "example.com");
  EXPECT_EQ(CoveringSite(sites, "www.Example.com."), "example.com");
  EXPECT_EQ(CoveringSite(sites, "notexample.com"), "");
  EXPECT_EQ(CoveringSite(sites, "example.com.evil.test"), "");
  SiteList list;
  EXPECT_TRUE(list.Add("www.shop.test"));
  EXPECT_FALSE(list.Add("www.shop.test"));
  EXPECT_TRUE(list.Covers("WWW.shop.test"));
  EXPECT_FALSE(list.Covers("shop.test"));
  EXPECT_TRUE(list.Add("shop.test"));
  EXPECT_FALSE(list.Add("a.shop.test"));  // Covered already.
  EXPECT_TRUE(list.Remove("www.shop.test"));  // Both entries covering it go.
  EXPECT_FALSE(list.Covers("www.shop.test"));
  EXPECT_FALSE(list.Remove("www.shop.test"));
  EXPECT_TRUE(list.Get().empty());
}

}  // namespace
}  // namespace torfox::adblock
