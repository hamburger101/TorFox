// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/bridge/validator.h"

#include <set>
#include <string>

#include "gtest/gtest.h"
#include "src/core/url/internal_url.h"

namespace torfox::bridge {
namespace {

Context Ctx(std::string_view url, bool persistent = false) {
  Context c;
  c.sender_is_internal = true;
  c.is_main_frame = true;
  c.frame_url = url;
  c.persistent = persistent;
  return c;
}

Reject Check(const Context& ctx, const std::string& msg) {
  return Validate(TorFoxPages(), ctx, msg).reject;
}

constexpr char kShell[] = "torfox://shell/";
constexpr char kSettings[] = "torfox://settings/";
constexpr char kNewTab[] = "torfox://newtab/";

TEST(BridgeValidator, AcceptsValidMessages) {
  Validated v = Validate(TorFoxPages(), Ctx(kShell), R"({"cmd":"tab.close","args":{"id":3}})");
  ASSERT_TRUE(v.ok()) << RejectName(v.reject) << " " << v.detail;
  EXPECT_EQ(v.page, "shell");
  EXPECT_EQ(v.command, "tab.close");
  EXPECT_EQ(GetInt(v.args, "id"), 3);

  Validated n = Validate(TorFoxPages(), Ctx(kShell), R"({"cmd":"tab.new"})");
  ASSERT_TRUE(n.ok());
  EXPECT_TRUE(n.args.is_object());

  EXPECT_EQ(Check(Ctx(kShell, true), R"({"cmd":"state.subscribe"})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kSettings), R"({"cmd":"settings.set","args":{"key":"a","value":[1]}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx("torfox://settings/?section=tor#x"), R"({"cmd":"settings.get"})"),
            Reject::kNone);
}

TEST(BridgeValidator, RejectsSendersThatAreNotInternalMainFrames) {
  Context c = Ctx(kShell);
  c.sender_is_internal = false;
  EXPECT_EQ(Check(c, R"({"cmd":"tab.new"})"), Reject::kNotInternalBrowser);
  c = Ctx(kShell);
  c.is_main_frame = false;
  EXPECT_EQ(Check(c, R"({"cmd":"tab.new"})"), Reject::kNotMainFrame);
}

TEST(BridgeValidator, RejectsForeignOrigins) {
  for (const char* url : {"https://example.com/", "http://127.0.0.1/", "about:blank",
                          "data:text/html,x", "TORFOX://shell/", "torfox://shell:1/",
                          "torfox://evil@shell/", "", "file:///C:/x", "blob:torfox://shell/x"}) {
    EXPECT_EQ(Check(Ctx(url), R"({"cmd":"tab.new"})"), Reject::kBadOrigin) << url;
  }
  EXPECT_EQ(Check(Ctx("torfox://evil/"), R"({"cmd":"tab.new"})"), Reject::kUnknownPage);
}

TEST(BridgeValidator, RejectsMalformedMessages) {
  for (const char* msg : {"", "not json", "[]", "null", "\"tab.new\"", "{}", R"({"cmd":1})",
                          R"({"cmd":""})", R"({"cmd":"tab.new","args":[]})",
                          R"({"cmd":"tab.new","args":"x"})", R"({"cmd":"tab.new","extra":1})",
                          R"({"cmd":"tab.new"} trailing)"}) {
    EXPECT_EQ(Check(Ctx(kShell), msg), Reject::kMalformed) << msg;
  }
  std::string deep = R"({"cmd":"settings.set","args":{"key":"k","value":[[[[[[1]]]]]]}})";
  EXPECT_EQ(Check(Ctx(kSettings), deep), Reject::kMalformed);
  std::string big = R"({"cmd":"nav.go","args":{"input":")" + std::string(kMaxMessageBytes, 'a') +
                    "\"}}";
  EXPECT_EQ(Check(Ctx(kShell), big), Reject::kTooLarge);
}

TEST(BridgeValidator, EnforcesPerPageAllowlists) {
  // Pages may only use their own commands.
  EXPECT_EQ(Check(Ctx(kSettings), R"({"cmd":"tab.close","args":{"id":1}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kNewTab), R"({"cmd":"settings.set","args":{"key":"a","value":1}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kNewTab), R"({"cmd":"tab.new"})"), Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"settings.set","args":{"key":"a","value":1}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"TAB.NEW"})"), Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"eval"})"), Reject::kUnknownCommand);
}

TEST(BridgeValidator, EnforcesPersistence) {
  EXPECT_EQ(Check(Ctx(kShell, false), R"({"cmd":"state.subscribe"})"),
            Reject::kPersistenceMismatch);
  EXPECT_EQ(Check(Ctx(kShell, true), R"({"cmd":"tab.new"})"), Reject::kPersistenceMismatch);
}

TEST(BridgeValidator, ValidatesArguments) {
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.close"})"), Reject::kMissingArgument);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.close","args":{"id":1,"x":2}})"),
            Reject::kUnknownArgument);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.new","args":{"x":1}})"), Reject::kUnknownArgument);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.close","args":{"id":"1"}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.close","args":{"id":1.5}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.close","args":{"id":true}})"),
            Reject::kBadArgumentType);
  for (const char* id : {"0", "-1", "2147483648", "18446744073709551615"}) {
    EXPECT_EQ(Check(Ctx(kShell), std::string(R"({"cmd":"tab.close","args":{"id":)") + id + "}}"),
              Reject::kArgumentOutOfRange)
        << id;
  }
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"nav.go","args":{"input":5}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"nav.go","args":{"input":"a\u0000b"}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"nav.go","args":{"input":"a\nb"}})"),
            Reject::kBadArgumentType);
  std::string long_input = R"({"cmd":"nav.go","args":{"input":")" + std::string(8193, 'a') + "\"}}";
  EXPECT_EQ(Check(Ctx(kShell), long_input), Reject::kArgumentOutOfRange);
  std::string long_key =
      R"({"cmd":"settings.set","args":{"key":")" + std::string(129, 'k') + R"(","value":1}})";
  EXPECT_EQ(Check(Ctx(kSettings), long_key), Reject::kArgumentOutOfRange);
}

TEST(BridgeValidator, BookmarksAndHistoryPagesHaveOnlyTheirOwnCommands) {
  constexpr char kBookmarks[] = "torfox://bookmarks/";
  constexpr char kHistory[] = "torfox://history/";
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"bookmarks.children","args":{"id":1}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx(kBookmarks),
                  R"({"cmd":"bookmarks.add","args":{"parent":1,"title":"A","url":"https://a.example/"}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"bookmarks.move","args":{"id":3,"parent":2,"index":-1}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"history.list","args":{"query":"x","limit":50}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"history.clear"})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kHistory, true), R"({"cmd":"history.subscribe"})"), Reject::kNone);

  // Neither page can drive tabs, settings, or the other page's data.
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"history.clear"})"), Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"bookmarks.remove","args":{"id":3}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"tab.close","args":{"id":1}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"settings.set","args":{"key":"a","value":1}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"nav.go","args":{"input":"x"}})"),
            Reject::kUnknownCommand);
  // History may not be cleared from the settings or new tab pages, nor the shell.
  for (const char* page : {kSettings, kNewTab, kShell}) {
    EXPECT_EQ(Check(Ctx(page), R"({"cmd":"history.clear"})"), Reject::kUnknownCommand) << page;
  }
}

TEST(BridgeValidator, ValidatesStageTwoArguments) {
  constexpr char kBookmarks[] = "torfox://bookmarks/";
  constexpr char kHistory[] = "torfox://history/";
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"zoom.change","args":{"step":2}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"zoom.change","args":{"step":-1}})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"tab.move","args":{"id":1,"index":-1}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"app.menu","args":{"x":-5,"y":3}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"find.query","args":{"text":"a","forward":"yes"}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"nav.open","args":{"url":"https://a/","newTab":1}})"),
            Reject::kBadArgumentType);
  // Bookmark ids are JavaScript-safe integers.
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"bookmarks.remove","args":{"id":9007199254740992}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kBookmarks), R"({"cmd":"bookmarks.remove","args":{"id":0}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kBookmarks),
                  R"({"cmd":"bookmarks.update","args":{"id":3,"title":"a\u0007"}})"),
            Reject::kBadArgumentType);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"history.list","args":{"limit":501}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kHistory), R"({"cmd":"history.list","args":{"before":-1}})"),
            Reject::kArgumentOutOfRange);
}

TEST(BridgeValidator, SuggestionListOnlyReportsAClickedRow) {
  constexpr char kSuggest[] = "torfox://suggest/";
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"suggest.open","args":{"index":0}})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"suggest.open","args":{"index":5}})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kSuggest, true), R"({"cmd":"suggest.subscribe"})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"suggest.open","args":{"index":6}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"suggest.open","args":{"index":-1}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"suggest.open","args":{"index":0,"url":"https://a/"}})"),
            Reject::kUnknownArgument);
  // The list cannot navigate, open tabs, read places or query suggestions itself.
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"nav.open","args":{"url":"https://a/"}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"nav.go","args":{"input":"x"}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kSuggest), R"({"cmd":"tab.new"})"), Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kSuggest),
                  R"({"cmd":"suggest.query","args":{"text":"a","x":0,"y":0,"width":10}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kSuggest, true), R"({"cmd":"state.subscribe"})"), Reject::kUnknownCommand);
  // The shell asks; other pages cannot.
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"suggest.query","args":{"text":"exa","x":120,"y":72,"width":600}})"),
            Reject::kNone);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"suggest.highlight","args":{"index":-1}})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"suggest.hide"})"), Reject::kNone);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"suggest.open","args":{"index":0}})"),
            Reject::kUnknownCommand);
  EXPECT_EQ(Check(Ctx(kShell),
                  R"({"cmd":"suggest.query","args":{"text":"a","x":0,"y":0,"width":0}})"),
            Reject::kArgumentOutOfRange);
  EXPECT_EQ(Check(Ctx(kShell), R"({"cmd":"suggest.query","args":{"text":")" +
                                   std::string(257, 'a') + R"(","x":0,"y":0,"width":10}})"),
            Reject::kArgumentOutOfRange);
  for (const char* page : {kSettings, kNewTab, "torfox://history/"}) {
    EXPECT_EQ(Check(Ctx(page), R"({"cmd":"suggest.open","args":{"index":0}})"),
              Reject::kUnknownCommand)
        << page;
  }
}

TEST(BridgeValidator, TablesAreConsistent) {
  for (const PageSpec& page : TorFoxPages()) {
    EXPECT_TRUE(IsInternalPage(page.host)) << page.host;
    std::set<std::string_view> names;
    for (const CommandSpec& c : page.commands) {
      EXPECT_TRUE(names.insert(c.name).second) << page.host << " " << c.name;
      std::set<std::string_view> args;
      for (const ArgSpec& a : c.args) {
        EXPECT_TRUE(args.insert(a.name).second) << c.name << " " << a.name;
      }
    }
  }
}

}  // namespace
}  // namespace torfox::bridge
