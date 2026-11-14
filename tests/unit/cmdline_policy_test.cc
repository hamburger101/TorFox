// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/cmdline/policy.h"

#include <algorithm>

#include "gtest/gtest.h"

namespace torfox::cmdline {
namespace {

bool Has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// The user's switches are also present in the processed command line, as
// they are when CEF calls OnBeforeCommandLineProcessing.
Input WithUser(SwitchMap user, SwitchMap cef_extra = {}) {
  Input in;
  in.user_switches = user;
  in.current_switches = user;
  for (auto& [k, v] : cef_extra) {
    in.current_switches[k] = v;
  }
  return in;
}

TEST(CmdlinePolicy, ReleaseDropsDangerousUserSwitches) {
  Input in = WithUser({{"no-sandbox", ""},
                       {"proxy-server", "socks5://evil:1080"},
                       {"remote-debugging-port", "9222"},
                       {"disable-web-security", ""},
                       {"user-data-dir", "C:\\x"},
                       {"renderer-cmd-prefix", "cmd /c calc"},
                       {"single-process", ""},
                       {"host-resolver-rules", "MAP * 1.2.3.4"},
                       {"torfox-test-log", "x"},
                       {"load-extension", "C:\\ext"}});
  Plan p = Apply(in);
  for (const char* s : {"no-sandbox", "proxy-server", "remote-debugging-port",
                        "disable-web-security", "user-data-dir", "renderer-cmd-prefix",
                        "single-process", "torfox-test-log", "load-extension"}) {
    EXPECT_EQ(p.switches.count(s), 0u) << s;
    EXPECT_TRUE(Has(p.removed, s)) << s;
  }
  // The user's resolver rules are replaced by TorFox's.
  EXPECT_EQ(p.switches.at("host-resolver-rules"), "MAP *.onion ~NOTFOUND");
}

TEST(CmdlinePolicy, KeepsCefOwnSwitches) {
  Input in = WithUser({{"no-sandbox", ""}}, {{"lang", "en-US"}, {"some-cef-switch", "1"}});
  Plan p = Apply(in);
  EXPECT_EQ(p.switches.at("lang"), "en-US");
  EXPECT_EQ(p.switches.at("some-cef-switch"), "1");
}

TEST(CmdlinePolicy, AddsForcedSwitches) {
  Plan p = Apply(Input{});
  for (const auto& [name, value] : ForcedSwitches("")) {
    ASSERT_EQ(p.switches.count(name), 1u) << name;
    EXPECT_EQ(p.switches.at(name), value) << name;
  }
  for (const char* s : {"site-per-process", "disable-extensions", "disable-background-networking",
                        "disable-component-update"}) {
    EXPECT_EQ(p.switches.count(s), 1u) << s;
  }
  EXPECT_EQ(p.switches.at("host-resolver-rules"), "MAP *.onion ~NOTFOUND");
  EXPECT_EQ(p.switches.at("component-updater"), "url-source=http://127.0.0.1:9/,disable-pings");
  EXPECT_EQ(p.switches.at("enable-features"), "NetworkServiceSandbox");
  EXPECT_EQ(SplitFeatureList(p.switches.at("disable-features")), ForcedDisableFeatures());
  EXPECT_TRUE(Has(ForcedDisableFeatures(), "HttpsUpgrades"));
  EXPECT_TRUE(Has(ForcedDisableFeatures(), "NetworkTimeServiceQuerying"));
}

TEST(CmdlinePolicy, UserCannotRedirectTheComponentUpdater) {
  Input in = WithUser({{"component-updater", "url-source=https://evil.test/"}});
  in.current_switches = {{"component-updater", "url-source=https://evil.test/"}};
  Plan p = Apply(in);
  EXPECT_EQ(p.switches.at("component-updater"), "url-source=http://127.0.0.1:9/,disable-pings");
}

TEST(CmdlinePolicy, MergesFeatureListsWithCefAndRemovesUserEntries) {
  // CEF's processed line already merged the user's entry into its own value.
  Input in;
  in.user_switches = {{"disable-features", "UserOff"}, {"enable-features", "UserOn"}};
  in.current_switches = {{"disable-features", "CefOff1,UserOff,CefOff2"},
                         {"enable-features", "UserOn"}};
  Plan p = Apply(in);
  auto disabled = SplitFeatureList(p.switches.at("disable-features"));
  EXPECT_TRUE(Has(disabled, "CefOff1"));
  EXPECT_TRUE(Has(disabled, "CefOff2"));
  EXPECT_TRUE(Has(disabled, "HttpsUpgrades"));
  EXPECT_FALSE(Has(disabled, "UserOff"));
  auto enabled = SplitFeatureList(p.switches.at("enable-features"));
  EXPECT_FALSE(Has(enabled, "UserOn"));
  EXPECT_TRUE(Has(enabled, "NetworkServiceSandbox"));
}

TEST(CmdlinePolicy, UserCannotDisableForcedFeatures) {
  Input in = WithUser({{"disable-features", "NetworkServiceSandbox"},
                       {"enable-features", "HttpsUpgrades"}});
  Plan p = Apply(in);
  EXPECT_FALSE(Has(SplitFeatureList(p.switches.at("disable-features")), "NetworkServiceSandbox"));
  EXPECT_TRUE(Has(SplitFeatureList(p.switches.at("enable-features")), "NetworkServiceSandbox"));
  EXPECT_FALSE(Has(SplitFeatureList(p.switches.at("enable-features")), "HttpsUpgrades"));
}

TEST(CmdlinePolicy, TestHooksAllowOnlyTheirSwitches) {
  Input in = WithUser({{"remote-debugging-port", "9333"},
                       {"torfox-test-log", "C:\\log.jsonl"},
                       {"torfox-test-host-rules", "MAP fixture.test 127.0.0.1"},
                       {"no-sandbox", ""},
                       {"proxy-server", "x"}});
  in.test_hooks = true;
  Plan p = Apply(in);
  EXPECT_EQ(p.switches.at("remote-debugging-port"), "9333");
  EXPECT_EQ(p.switches.at("torfox-test-log"), "C:\\log.jsonl");
  EXPECT_EQ(p.switches.count("no-sandbox"), 0u);
  EXPECT_EQ(p.switches.count("proxy-server"), 0u);
  // Test rules come after the onion rule, so they can never override it.
  EXPECT_EQ(p.switches.at("host-resolver-rules"),
            "MAP *.onion ~NOTFOUND,MAP fixture.test 127.0.0.1");
}

TEST(CmdlinePolicy, KeepsOnlyHttpUrlArguments) {
  Input in;
  in.user_args = {"https://example.com/", "HTTP://example.org/x", "file:///C:/x", "javascript:x",
                  "C:\\Users\\x.html", "--", "https://", "https://a b", "torfox://settings/"};
  Plan p = Apply(in);
  EXPECT_EQ(p.args, (std::vector<std::string>{"https://example.com/", "HTTP://example.org/x"}));
  EXPECT_EQ(p.dropped_args.size(), 7u);
}

TEST(CmdlinePolicy, SplitAndJoin) {
  EXPECT_EQ(SplitFeatureList(" a, ,b,,c "), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_TRUE(SplitFeatureList("").empty());
  EXPECT_EQ(JoinFeatureList({"a", "b"}), "a,b");
}

}  // namespace
}  // namespace torfox::cmdline
