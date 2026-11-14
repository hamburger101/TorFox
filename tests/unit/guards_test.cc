// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/guards.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace torfox::guards {
namespace {

constexpr const char* kOnion = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";

TEST(GuardsTest, ParsesSchemeAndHost) {
  EXPECT_EQ(SchemeOf("HTTPS://Example.com/"), "https");
  EXPECT_EQ(SchemeOf("no-scheme"), "");
  EXPECT_EQ(SchemeOf(":nothing"), "");
  EXPECT_EQ(HostOf("https://Example.COM:8443/path"), "example.com");
  EXPECT_EQ(HostOf("http://user:pw@host.test/"), "host.test");
  EXPECT_EQ(HostOf("http://a@b@evil.test/"), "evil.test");
  EXPECT_EQ(HostOf("http://[::1]:8080/"), "::1");
  EXPECT_EQ(HostOf("http://host.test?q"), "host.test");
  EXPECT_EQ(HostOf("http://host.test#f"), "host.test");
  EXPECT_EQ(HostOf("http://host.test\\path"), "host.test");
  EXPECT_EQ(HostOf("data:text/html,x"), "");
}

TEST(GuardsTest, RecognisesOnionHosts) {
  EXPECT_TRUE(IsOnionHost(kOnion));
  EXPECT_TRUE(IsOnionHost("sub.example.onion"));
  EXPECT_TRUE(IsOnionHost("EXAMPLE.ONION"));
  EXPECT_TRUE(IsOnionHost("example.onion."));
  EXPECT_TRUE(IsOnionHost("example.onion.."));
  EXPECT_FALSE(IsOnionHost("onion"));
  EXPECT_FALSE(IsOnionHost(".onion"));
  EXPECT_FALSE(IsOnionHost("onion.com"));
  EXPECT_FALSE(IsOnionHost("example.onion.com"));
  EXPECT_FALSE(IsOnionHost("exampleonion"));
}

TEST(GuardsTest, InternalBrowsersShowOnlyTheirOwnPages) {
  EXPECT_EQ(NavigationBlockReason(Role::kShell, "torfox://shell/"), "");
  EXPECT_EQ(NavigationBlockReason(Role::kInternalTab, "torfox://newtab/"), "");
  EXPECT_EQ(NavigationBlockReason(Role::kInternalTab, "torfox://settings/"), "");
  // Role mismatch.
  EXPECT_NE(NavigationBlockReason(Role::kShell, "torfox://settings/"), "");
  EXPECT_NE(NavigationBlockReason(Role::kInternalTab, "torfox://shell/"), "");
  // Unknown pages and malformed URLs.
  for (const char* url : {"torfox://evil/", "torfox://Settings/", "TORFOX://settings/",
                          "torfox://settings:80/", "torfox://user@settings/", "torfox:settings",
                          "torfox://", ""}) {
    EXPECT_NE(NavigationBlockReason(Role::kInternalTab, url), "") << url;
    EXPECT_NE(NavigationBlockReason(Role::kShell, url), "") << url;
  }
  // Never the web.
  for (const char* url : {"https://example.com/", "http://127.0.0.1/", "data:text/html,x",
                          "blob:torfox://settings/1", "about:blank", "file:///C:/", "javascript:1"}) {
    EXPECT_NE(NavigationBlockReason(Role::kInternalTab, url), "") << url;
    EXPECT_NE(NavigationBlockReason(Role::kShell, url), "") << url;
  }
}

TEST(GuardsTest, WebTabsNeverReachPrivilegedSchemes) {
  for (const char* url :
       {"torfox://settings/", "torfox://shell/", "TORFOX://settings/", "torfox:settings",
        " torfox://settings/", "\ttorfox://newtab/", "tor\nfox://settings/", "torfox://evil/",
        "chrome://settings/", "chrome-untrusted://x/", "devtools://devtools/", "chrome-error://x/",
        "chrome-extension://abc/", "chrome-search://x/", "file:///C:/Windows/", "view-source:https://a/",
        "filesystem:https://a/temporary/x", "blob:torfox://settings/1", "BLOB:torfox://settings/1",
        "blob:chrome://settings/1", "blob:devtools://x/1", "blob: torfox://newtab/1"}) {
    EXPECT_NE(NavigationBlockReason(Role::kWebTab, url), "") << url;
    EXPECT_NE(ResourceBlockReason(Role::kWebTab, url), "") << url;
  }
  EXPECT_EQ(ResourceBlockReason(Role::kWebTab, "blob:https://example.com/1"), "");
  EXPECT_EQ(ResourceBlockReason(Role::kWebTab, "blob:null/1"), "");
}

TEST(GuardsTest, WebTabsAllowOrdinaryContent) {
  for (const char* url : {"https://example.com/", "http://127.0.0.1:8080/x", "data:text/html,x",
                          "blob:https://example.com/123", "about:blank", "about:srcdoc"}) {
    EXPECT_EQ(NavigationBlockReason(Role::kWebTab, url), "") << url;
  }
  EXPECT_NE(NavigationBlockReason(Role::kWebTab, "about:settings"), "");
  EXPECT_NE(NavigationBlockReason(Role::kWebTab, "javascript:alert(1)"), "");
  EXPECT_NE(NavigationBlockReason(Role::kWebTab, "mailto:a@b.test"), "");
  EXPECT_NE(NavigationBlockReason(Role::kWebTab, "ftp://example.com/"), "");
  EXPECT_EQ(ResourceBlockReason(Role::kWebTab, "https://cdn.example.com/a.js"), "");
  EXPECT_EQ(ResourceBlockReason(Role::kWebTab, "wss://example.com/socket"), "");
}

TEST(GuardsTest, WebTabsNeverReachOnionHosts) {
  const std::string onion = kOnion;
  for (const std::string& url : std::vector<std::string>{
           "http://" + onion + "/", "https://" + onion + "/path", "https://sub." + onion + ":443/",
           "http://user@" + onion + "/", "HTTP://" + onion + "./"}) {
    EXPECT_NE(NavigationBlockReason(Role::kWebTab, url), "") << url;
    EXPECT_NE(ResourceBlockReason(Role::kWebTab, url), "") << url;
  }
  EXPECT_NE(ResourceBlockReason(Role::kWebTab, "wss://" + onion + "/s"), "");
  EXPECT_NE(ResourceBlockReason(Role::kWebTab, "ws://" + onion + "/s"), "");
  EXPECT_EQ(NavigationBlockReason(Role::kWebTab, "https://onion.example.com/"), "");
}

TEST(GuardsTest, InternalPagesLoadOnlyBundledResources) {
  EXPECT_EQ(ResourceBlockReason(Role::kInternalTab, "torfox://newtab/assets/a.js"), "");
  EXPECT_EQ(ResourceBlockReason(Role::kShell, "torfox://shell/assets/a.css"), "");
  EXPECT_EQ(ResourceBlockReason(Role::kInternalTab, "data:image/png;base64,AA=="), "");
  EXPECT_EQ(ResourceBlockReason(Role::kInternalTab, "blob:torfox://settings/1"), "");
  for (const char* url : {"https://example.com/", "http://127.0.0.1/", "ws://127.0.0.1/",
                          "file:///C:/x", "chrome://settings/", "blob:https://example.com/1",
                          "blob:null/1", "filesystem:torfox://settings/temporary/x"}) {
    EXPECT_NE(ResourceBlockReason(Role::kInternalTab, url), "") << url;
    EXPECT_NE(ResourceBlockReason(Role::kShell, url), "") << url;
  }
}

TEST(GuardsTest, SchemeHandlerServesOnlyInternalBrowsers) {
  auto ok = DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://newtab/");
  EXPECT_EQ(ok.status, 200);
  EXPECT_EQ(ok.resource_path, "pages/newtab/index.html");
  EXPECT_EQ(DecideInternalRequest(true, Role::kShell, "GET", "torfox://shell/assets/x.js").resource_path,
            "assets/x.js");

  // Web tabs and unknown browsers get nothing, whatever they ask for.
  for (const char* url : {"torfox://newtab/", "torfox://settings/", "torfox://shell/",
                          "torfox://newtab/assets/x.js"}) {
    EXPECT_EQ(DecideInternalRequest(true, Role::kWebTab, "GET", url).status, 403) << url;
    EXPECT_EQ(DecideInternalRequest(false, Role::kInternalTab, "GET", url).status, 403) << url;
    EXPECT_EQ(DecideInternalRequest(false, Role::kShell, "GET", url).status, 403) << url;
    EXPECT_TRUE(DecideInternalRequest(true, Role::kWebTab, "GET", url).resource_path.empty());
  }
  // Only GET.
  for (const char* method : {"POST", "PUT", "get", "HEAD", ""}) {
    EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, method, "torfox://newtab/").status, 405)
        << method;
  }
  // The shell page belongs to the shell alone.
  EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://shell/").status, 403);
  EXPECT_EQ(DecideInternalRequest(true, Role::kShell, "GET", "torfox://settings/").status, 403);
  // Unknown pages and resources.
  EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://evil/").status, 404);
  EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://newtab/../shell/").status,
            404);
  EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://newtab/assets/../x").status,
            404);
  EXPECT_EQ(DecideInternalRequest(true, Role::kInternalTab, "GET", "torfox://newtab/secret.txt").status,
            404);
}

}  // namespace
}  // namespace torfox::guards
