// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/nav_input.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace torfox {
namespace {

NavDecision C(const std::string& s) {
  return ClassifyNavInput(s);
}

TEST(NavInput, UrlsAndHosts) {
  EXPECT_EQ(C("https://example.com/a?b=c").kind, NavKind::kWebUrl);
  EXPECT_EQ(C("https://example.com/a?b=c").url, "https://example.com/a?b=c");
  EXPECT_EQ(C("HTTP://Example.org").kind, NavKind::kWebUrl);
  EXPECT_EQ(C("example.com").url, "https://example.com");
  EXPECT_EQ(C("  example.com/path  ").url, "https://example.com/path");
  EXPECT_EQ(C("sub.example.co.uk/x?y").kind, NavKind::kWebUrl);
  EXPECT_EQ(C("example.com.").url, "https://example.com.");
  EXPECT_EQ(C("example.com:8443/x").url, "https://example.com:8443/x");
  EXPECT_EQ(C("http://[::1]:8080/").kind, NavKind::kWebUrl);
}

TEST(NavInput, OnlyKnownTopLevelDomainsAreHosts) {
  // Public Suffix List TLDs, in ASCII or Unicode form.
  EXPECT_EQ(C("example.org").kind, NavKind::kWebUrl);
  EXPECT_EQ(C("readme.md").kind, NavKind::kWebUrl);  // .md is Moldova.
  // "\u043f\u0440\u0438\u043c\u0435\u0440.\u0440\u0444" (a Cyrillic name), as UTF-8 bytes.
  EXPECT_EQ(C("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x80.\xD1\x80\xD1\x84").kind,
            NavKind::kWebUrl);
  // Unknown TLDs are searches...
  EXPECT_EQ(C("node.js").kind, NavKind::kSearch);
  EXPECT_EQ(C("index.html").kind, NavKind::kSearch);
  EXPECT_EQ(C("nas.lan").kind, NavKind::kSearch);
  // ...unless a port or path says the user means a host.
  EXPECT_EQ(C("nas.lan:8080").url, "https://nas.lan:8080");
  EXPECT_EQ(C("nas.lan/").url, "https://nas.lan/");
  EXPECT_EQ(C("nas:8080").url, "https://nas:8080");
  EXPECT_EQ(C("nas").kind, NavKind::kSearch);
  EXPECT_EQ(C("nas/").kind, NavKind::kSearch);
  // Numbers are not addresses.
  EXPECT_EQ(C("1.5").kind, NavKind::kSearch);
  EXPECT_EQ(C("3.14.15").kind, NavKind::kSearch);
  EXPECT_EQ(C("1.2.3.4.5").kind, NavKind::kSearch);
  EXPECT_EQ(C("256.1.1.1").kind, NavKind::kSearch);
  // Malformed hosts and ports.
  EXPECT_EQ(C("example..com").kind, NavKind::kSearch);
  EXPECT_EQ(C(".example.com").kind, NavKind::kSearch);
  EXPECT_EQ(C("example.com:99999").kind, NavKind::kSearch);
  EXPECT_EQ(C("example.com:http").kind, NavKind::kSearch);
}

TEST(NavInput, LocalHostsUseHttpEverythingElseHttps) {
  EXPECT_EQ(C("localhost:8080").url, "http://localhost:8080");
  EXPECT_EQ(C("localhost").url, "http://localhost");
  EXPECT_EQ(C("app.localhost:3000/x").url, "http://app.localhost:3000/x");
  EXPECT_EQ(C("127.0.0.1").url, "http://127.0.0.1");
  EXPECT_EQ(C("192.168.1.1").url, "http://192.168.1.1");
  EXPECT_EQ(C("10.0.0.8:8006").url, "http://10.0.0.8:8006");
  EXPECT_EQ(C("172.20.0.1").url, "http://172.20.0.1");
  EXPECT_EQ(C("169.254.1.1").url, "http://169.254.1.1");
  EXPECT_EQ(C("[::1]:8080").url, "http://[::1]:8080");
  EXPECT_EQ(C("[fe80::1]").url, "http://[fe80::1]");
  // Public addresses and names never get a plain-text default.
  EXPECT_EQ(C("8.8.8.8").url, "https://8.8.8.8");
  EXPECT_EQ(C("172.32.0.1").url, "https://172.32.0.1");
  EXPECT_EQ(C("[2001:db8::1]").url, "https://[2001:db8::1]");
  EXPECT_EQ(C("localhost.example.com").url, "https://localhost.example.com");
  EXPECT_EQ(C("nas.lan:8080").url, "https://nas.lan:8080");
}

TEST(NavInput, SearchGoesToDuckDuckGo) {
  NavDecision d = C("hello world");
  EXPECT_EQ(d.kind, NavKind::kSearch);
  EXPECT_EQ(d.url, "https://duckduckgo.com/?q=hello%20world");
  EXPECT_EQ(C("foo").url, "https://duckduckgo.com/?q=foo");
  EXPECT_EQ(C("a&b=c#d").url, "https://duckduckgo.com/?q=a%26b%3Dc%23d");
  EXPECT_EQ(C("user@example.com").kind, NavKind::kSearch);
  EXPECT_EQ(C("onion soup recipe").kind, NavKind::kSearch);
  EXPECT_EQ(C("what is onion").kind, NavKind::kSearch);
  EXPECT_EQ(C("theonion.com").kind, NavKind::kWebUrl);
  // Non-ASCII is percent-encoded as UTF-8.
  EXPECT_EQ(C("caf\xC3\xA9 menu").url, "https://duckduckgo.com/?q=caf%C3%A9%20menu");
}

TEST(NavInput, OnionInputIsNeverSearched) {
  const std::string v3 = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";
  const std::vector<std::string> inputs = {
      v3, "http://" + v3 + "/", "https://" + v3 + "/path?q=1", v3 + ":80",
      "user@" + v3, "FOO.ONION", "foo.onion.", "please visit " + v3 + " now",
      "(" + v3 + ")", "\"" + v3 + "\"", "go to http://abc.onion/x?",
      "short.onion",  // Malformed onions are still kept away from search.
  };
  for (const std::string& s : inputs) {
    NavDecision d = C(s);
    EXPECT_EQ(d.kind, NavKind::kOnion) << s;
    EXPECT_EQ(d.url.find("duckduckgo.com/?q="), std::string::npos) << s;
  }
}

TEST(NavInput, RefusesDangerousSchemesAndInput) {
  for (const char* s : {"javascript:alert(1)", "JavaScript:alert(1)", "data:text/html,<b>x</b>",
                        "file:///C:/Windows/win.ini", "chrome://settings", "devtools://x",
                        "ftp://example.com/", "view-source:https://example.com", "vbscript:x",
                        "blob:https://example.com/uuid", "about:config"}) {
    NavDecision d = C(s);
    EXPECT_EQ(d.kind, NavKind::kRejected) << s;
    EXPECT_TRUE(d.url.empty()) << s;
  }
  EXPECT_EQ(C("").kind, NavKind::kRejected);
  EXPECT_EQ(C("   ").kind, NavKind::kRejected);
  EXPECT_EQ(C(std::string("a\x01" "b")).kind, NavKind::kRejected);
  EXPECT_EQ(C(std::string(kMaxNavInputLength + 1, 'a')).kind, NavKind::kRejected);
  EXPECT_EQ(C("http://").kind, NavKind::kRejected);
}

TEST(NavInput, InternalPages) {
  EXPECT_EQ(C("torfox://settings").kind, NavKind::kInternal);
  EXPECT_EQ(C("torfox://settings").url, "torfox://settings/");
  EXPECT_EQ(C("TORFOX://SETTINGS/").url, "torfox://settings/");
  EXPECT_EQ(C("torfox://newtab/").kind, NavKind::kInternal);
  // The shell page is the window chrome, never tab content.
  EXPECT_EQ(C("torfox://shell/").kind, NavKind::kRejected);
  // Nor is the address bar's suggestion list.
  EXPECT_EQ(C("torfox://suggest/").kind, NavKind::kRejected);
  EXPECT_EQ(C("TORFOX://SUGGEST/").kind, NavKind::kRejected);
  EXPECT_EQ(C("torfox://unknown/").kind, NavKind::kRejected);
}

TEST(NavInput, MentionsOnionHost) {
  EXPECT_TRUE(MentionsOnionHost("x.onion"));
  EXPECT_TRUE(MentionsOnionHost("a b c.onion/d"));
  EXPECT_FALSE(MentionsOnionHost("onion"));
  EXPECT_FALSE(MentionsOnionHost(".onion"));
  EXPECT_FALSE(MentionsOnionHost("example.com/x.onion"));  // Path, not host.
  EXPECT_FALSE(MentionsOnionHost(""));
}

}  // namespace
}  // namespace torfox
