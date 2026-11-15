// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "src/core/privacy/clear_data.h"
#include "src/core/privacy/external_protocols.h"
#include "src/core/privacy/https_first.h"
#include "src/core/privacy/trackers.h"

namespace torfox {
namespace {

// ----- trackers ---------------------------------------------------------------

TEST(Trackers, ListIsCompiledIn) {
  EXPECT_GT(trackers::ListSize(), 10000u);
  EXPECT_EQ(trackers::ListCommit().size(), 40u);
}

TEST(Trackers, MatchesListedDomainsAndTheirSubdomains) {
  EXPECT_EQ(trackers::MatchListed("google-analytics.com"), "google-analytics.com");
  EXPECT_EQ(trackers::MatchListed("www.google-analytics.com"), "google-analytics.com");
  EXPECT_EQ(trackers::MatchListed("SSL.Google-Analytics.COM."), "google-analytics.com");
  EXPECT_EQ(trackers::MatchListed("b.scorecardresearch.com"), "scorecardresearch.com");
  EXPECT_FALSE(trackers::MatchListed("googletagmanager.com").empty());
}

TEST(Trackers, DoesNotMatchLookalikesOrUnlisted) {
  EXPECT_TRUE(trackers::MatchListed("notgoogle-analytics.com").empty());
  EXPECT_TRUE(trackers::MatchListed("google-analytics.com.example.org").empty());
  EXPECT_TRUE(trackers::MatchListed("example.org").empty());
  EXPECT_TRUE(trackers::MatchListed("duckduckgo.com").empty());
  EXPECT_TRUE(trackers::MatchListed("com").empty());
  EXPECT_TRUE(trackers::MatchListed("").empty());
  EXPECT_TRUE(trackers::MatchListed("127.0.0.1").empty());
  EXPECT_TRUE(trackers::MatchListed("::1").empty());
}

TEST(Trackers, OnlyThirdPartyRequestsAreBlocked) {
  EXPECT_TRUE(trackers::Check("www.google-analytics.com", "news.example.co.uk").block);
  EXPECT_EQ(trackers::Check("www.google-analytics.com", "news.example.co.uk").listed,
            "google-analytics.com");
  // The tracker's own site, visited directly.
  EXPECT_FALSE(trackers::Check("www.google-analytics.com", "google-analytics.com").block);
  EXPECT_FALSE(trackers::Check("a.google-analytics.com", "b.google-analytics.com").block);
  // Not listed: never blocked.
  EXPECT_FALSE(trackers::Check("cdn.example.net", "news.example.co.uk").block);
  // No page yet: treated as another site.
  EXPECT_TRUE(trackers::Check("google-analytics.com", "").block);
}

TEST(Trackers, SitesFollowThePublicSuffixList) {
  EXPECT_EQ(trackers::SiteOf("a.b.example.co.uk"), "example.co.uk");
  EXPECT_EQ(trackers::SiteOf("WWW.Example.COM."), "example.com");
  EXPECT_EQ(trackers::SiteOf("user.github.io"), "user.github.io");  // A private suffix.
  EXPECT_EQ(trackers::SiteOf("127.0.0.1"), "127.0.0.1");
  EXPECT_EQ(trackers::SiteOf("localhost"), "localhost");
  EXPECT_EQ(trackers::SiteOf("co.uk"), "co.uk");
}

// ----- HTTPS-first ------------------------------------------------------------

TEST(HttpsFirst, UpgradesPublicHttpUrls) {
  EXPECT_EQ(https_first::UpgradedUrl("http://example.com/"), "https://example.com/");
  EXPECT_EQ(https_first::UpgradedUrl("http://www.example.co.uk/a/b?q=1#frag"),
            "https://www.example.co.uk/a/b?q=1#frag");
  EXPECT_EQ(https_first::UpgradedUrl("HTTP://Example.COM/x"), "https://Example.COM/x");
  EXPECT_EQ(https_first::UpgradedUrl("http://example.com"), "https://example.com/");
}

TEST(HttpsFirst, LeavesOtherUrlsAlone) {
  for (const char* url : {
           "https://example.com/",         // Already https.
           "ftp://example.com/",           // Not http.
           "http://example.com:8080/",     // An explicit port.
           "http://user:pw@example.com/",  // User info.
           "http://localhost/",            // Local names.
           "http://printer/",
           "http://nas.local/",
           "http://site.test/",
           "http://intranet.internal/",
           "http://foo.localhost/",
           "http://192.168.1.1/",  // IP addresses.
           "http://127.0.0.1/",
           "http://[::1]/",
           "http://co.uk/",  // A public suffix itself.
           "http://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/",
           "http://",
           "",
       }) {
    EXPECT_EQ(https_first::UpgradedUrl(url), "") << url;
  }
}

TEST(HttpsFirst, TestPortsMapOnlyTheGivenPort) {
  const https_first::TestPorts ports{"8080", "8443"};
  EXPECT_EQ(https_first::UpgradedUrl("http://example.com:8080/p", &ports), "https://example.com:8443/p");
  EXPECT_EQ(https_first::UpgradedUrl("http://example.com:9090/p", &ports), "");
  EXPECT_EQ(https_first::UpgradedUrl("http://site.test:8080/p", &ports), "");
}

TEST(HttpsFirst, FallbackIsOfferedOnlyWhenHttpMightWork) {
  EXPECT_TRUE(https_first::OffersHttpAfter(-102));  // ERR_CONNECTION_REFUSED
  EXPECT_TRUE(https_first::OffersHttpAfter(-200));  // ERR_CERT_COMMON_NAME_INVALID
  EXPECT_TRUE(https_first::OffersHttpAfter(-107));  // ERR_SSL_PROTOCOL_ERROR
  EXPECT_TRUE(https_first::OffersHttpAfter(-118));  // ERR_CONNECTION_TIMED_OUT
  EXPECT_FALSE(https_first::OffersHttpAfter(0));
  EXPECT_FALSE(https_first::OffersHttpAfter(-3));    // ERR_ABORTED
  EXPECT_FALSE(https_first::OffersHttpAfter(-105));  // ERR_NAME_NOT_RESOLVED
  EXPECT_FALSE(https_first::OffersHttpAfter(-106));  // ERR_INTERNET_DISCONNECTED
}

TEST(HttpsFirst, ExceptionsAreExactHosts) {
  https_first::Exceptions e;
  e.Add("Old.Example.com.");
  EXPECT_TRUE(e.Contains("old.example.com"));
  EXPECT_FALSE(e.Contains("example.com"));
  EXPECT_FALSE(e.Contains("www.old.example.com"));
  e.Clear();
  EXPECT_FALSE(e.Contains("old.example.com"));
}

// ----- external apps ----------------------------------------------------------

TEST(ExternalProtocols, OnlyMailAndPhoneLinksMayOpenAnApp) {
  EXPECT_EQ(external_protocols::LaunchableScheme("mailto:someone@example.com"), "mailto");
  EXPECT_EQ(external_protocols::LaunchableScheme("mailto:a@example.com?subject=hi%20there&body=x"),
            "mailto");
  EXPECT_EQ(external_protocols::LaunchableScheme("tel:+1-555-0100"), "tel");
  for (const char* url : {"ms-settings:privacy", "file:///C:/Windows/System32/calc.exe",
                          "javascript:alert(1)", "search-ms:query=x", "steam://run/1",
                          "mailtox:a@b.c", "telnet://host", "mailto:", "tel:", "", "MAILTO:a@b.c"}) {
    EXPECT_EQ(external_protocols::LaunchableScheme(url), "") << url;
  }
}

TEST(ExternalProtocols, RefusesUrlsACommandLineCouldSplit) {
  for (const char* url : {"mailto:a@b.c\" --evil", "mailto:a@b.c --evil", "mailto:a@b.c\t-x",
                          "mailto:a@b.c\n", "tel:1'2", "mailto:a@b.c|calc", "mailto:a@b.c^&x",
                          "mailto:a@b.c\\..", "mailto:\xC3\xA9@b.c", "mailto:a@b.c`x`"}) {
    EXPECT_EQ(external_protocols::LaunchableScheme(url), "") << url;
  }
  // Each refused character on its own, with nothing else wrong in the URL.
  for (char c : std::string("\"'\\<>^`{|}")) {
    const std::string url = std::string("mailto:a") + c + "b@example.com";
    EXPECT_EQ(external_protocols::LaunchableScheme(url), "") << url;
  }
  const std::string longest = "mailto:" + std::string(external_protocols::kMaxUrlLength - 7, 'a');
  EXPECT_EQ(external_protocols::LaunchableScheme(longest), "mailto");
  EXPECT_EQ(external_protocols::LaunchableScheme(longest + "a"), "");
}

// ----- clear on exit ------------------------------------------------------------

TEST(ClearData, RemovesOnlyTheChosenCategories) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("torfox-clear-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto touch = [&](const char* rel) {
    const std::filesystem::path p = dir / std::filesystem::path(rel).make_preferred();
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "data";
  };
  touch("Network/Cookies");
  touch("Local Storage/leveldb/000003.log");
  touch("IndexedDB/https_site.example_0.indexeddb.leveldb/LOG");
  touch("Cache/Cache_Data/data_0");
  touch("Code Cache/js/index");
  touch("Network/Network Persistent State");
  touch("Favicons");
  touch("Preferences");
  touch("Network/TransportSecurity");

  clear_data::Categories c;
  c.cookies = true;
  c.cache = true;
  const clear_data::Result r = clear_data::Clear(dir, c);
  EXPECT_EQ(r.failed, 0);
  EXPECT_EQ(r.removed, 6);
  for (const char* gone : {"Network/Cookies", "Local Storage", "IndexedDB", "Cache", "Code Cache",
                           "Network/Network Persistent State"}) {
    EXPECT_FALSE(std::filesystem::exists(dir / std::filesystem::path(gone).make_preferred())) << gone;
  }
  // Not chosen (history), or never cleared.
  for (const char* kept : {"Favicons", "Preferences", "Network/TransportSecurity"}) {
    EXPECT_TRUE(std::filesystem::exists(dir / std::filesystem::path(kept).make_preferred())) << kept;
  }
  c = clear_data::Categories();
  c.history = true;
  EXPECT_EQ(clear_data::Clear(dir, c).removed, 1);
  EXPECT_FALSE(std::filesystem::exists(dir / "Favicons"));
  EXPECT_EQ(clear_data::Clear(dir / "absent", c).removed, 0);
  EXPECT_TRUE(clear_data::PathsFor(clear_data::Categories()).empty());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

#if defined(_WIN32)
TEST(ClearData, NeverFollowsJunctions) {
  // A directory junction needs no special rights on Windows, so anything
  // that can write to the profile could plant one. Clearing removes the
  // junction, never what it points to.
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("torfox-clear-junction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::filesystem::path profile = root / "profile";
  const std::filesystem::path outside = root / "outside";
  std::filesystem::create_directories(profile / "Network");
  std::filesystem::create_directories(outside);
  std::ofstream(outside / "precious.txt") << "keep me";
  for (const char* rel : {"Cache", "IndexedDB", "Network\\Cookies"}) {
    const std::string cmd = "mklink /J \"" + (profile / rel).string() + "\" \"" + outside.string() +
                            "\" >NUL";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
  }
  clear_data::Categories c;
  c.cache = true;
  c.cookies = true;
  const clear_data::Result r = clear_data::Clear(profile, c);
  EXPECT_EQ(r.failed, 0);
  EXPECT_EQ(r.removed, 3);
  for (const char* rel : {"Cache", "IndexedDB", "Network\\Cookies"}) {
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(profile / rel))) << rel;
  }
  EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
#endif

TEST(ClearData, NeverFollowsLinks) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("torfox-clear-link-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::filesystem::path profile = root / "profile";
  const std::filesystem::path outside = root / "outside";
  std::filesystem::create_directories(profile);
  std::filesystem::create_directories(outside);
  std::ofstream(outside / "precious.txt") << "keep me";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, profile / "Cache", ec);
  if (ec) {
    std::filesystem::remove_all(root, ec);
    GTEST_SKIP() << "cannot create links here: " << ec.message();
  }
  clear_data::Categories c;
  c.cache = true;
  EXPECT_EQ(clear_data::Clear(profile, c).removed, 1);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(profile / "Cache")));
  EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));
  std::filesystem::remove_all(root, ec);
}

}  // namespace
}  // namespace torfox
