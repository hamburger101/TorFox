// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/places.h"

#include <fstream>
#include <iterator>
#include <string>

#include "gtest/gtest.h"
#include "sqlite3.h"
#include "tests/unit/test_util.h"

namespace torfox::browsing {
namespace {

using testing::ScopedTempDir;

std::unique_ptr<Places> OpenAt(const std::filesystem::path& path,
                               Places::OpenStatus* status = nullptr) {
  Places::OpenStatus s;
  std::string error;
  auto places = Places::Open(path, 1000, status ? status : &s, &error);
  EXPECT_TRUE(places) << error;
  return places;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Everything TorFox keeps for one database: the file and its journals.
std::string ReadDatabaseFiles(const std::filesystem::path& path) {
  std::string all;
  for (const char* extra : {"", "-wal", "-shm", "-journal"}) {
    std::filesystem::path p = path;
    p += extra;
    if (std::filesystem::exists(p)) {
      all += ReadAll(p);
    }
  }
  return all;
}

TEST(PlacesUrls, OnlyWebUrlsAreStored) {
  EXPECT_TRUE(IsStorableUrl("https://example.com/"));
  EXPECT_TRUE(IsStorableUrl("HTTP://example.com/a?b#c"));
  for (const char* bad : {"javascript:alert(1)", "data:text/html,x", "torfox://settings/",
                          "file:///C:/x", "chrome://version", "about:blank", "https://",
                          "https:///path", "http://exa mple.com/", "", "ftp://example.com/"}) {
    EXPECT_FALSE(IsStorableUrl(bad)) << bad;
  }
  EXPECT_FALSE(IsStorableUrl("https://example.com/" + std::string(kMaxUrlLength, 'a')));
}

TEST(PlacesHistory, RecordsVisitsNewestFirst) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  EXPECT_TRUE(p->AddVisit("https://a.example/", "A", 100));
  EXPECT_TRUE(p->AddVisit("https://b.example/", "B", 200));
  EXPECT_TRUE(p->AddVisit("https://a.example/", "", 300));  // Empty title keeps "A".
  auto recent = p->RecentHistory(10);
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent[0].url, "https://a.example/");
  EXPECT_EQ(recent[0].title, "A");
  EXPECT_EQ(recent[0].visit_count, 2);
  EXPECT_EQ(recent[0].last_visit, 300);
  EXPECT_EQ(recent[1].url, "https://b.example/");
  EXPECT_EQ(p->RecentHistory(10, 300).size(), 1u);  // Paging: before 300.
  EXPECT_TRUE(p->SetTitle("https://b.example/", "Bee"));
  EXPECT_EQ(p->GetHistory("https://b.example/")->title, "Bee");
  EXPECT_FALSE(p->SetTitle("https://never.example/", "x"));
}

TEST(PlacesHistory, RefusesNonWebUrls) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  EXPECT_FALSE(p->AddVisit("torfox://settings/", "Settings", 1));
  EXPECT_FALSE(p->AddVisit("data:text/html,x", "", 1));
  EXPECT_FALSE(p->AddVisit("chrome-error://chromewebdata/", "", 1));
  EXPECT_TRUE(p->RecentHistory(10).empty());
}

TEST(PlacesHistory, TitlesAreCleanedAndBounded) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  ASSERT_TRUE(p->AddVisit("https://a.example/", std::string("a\nb\x01" "c\xFF"), 1));
  EXPECT_EQ(p->GetHistory("https://a.example/")->title, "a b c\xEF\xBF\xBD");
  // Cut on a character boundary: 600 two-byte characters fit 512 times.
  std::string long_title;
  for (int i = 0; i < 600; ++i) {
    long_title += "\xC3\xA9";
  }
  ASSERT_TRUE(p->AddVisit("https://b.example/", long_title, 2));
  const std::string stored = p->GetHistory("https://b.example/")->title;
  EXPECT_EQ(stored.size(), kMaxTitleLength);
  EXPECT_EQ(stored.substr(stored.size() - 2), "\xC3\xA9");
}

TEST(PlacesHistory, SearchMatchesTitleAndUrlLiterally) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  p->AddVisit("https://news.example/rust", "Rust 2.0 released", 1);
  p->AddVisit("https://example.org/100%25", "Percent", 2);
  p->AddVisit("https://example.org/a_b", "Underscore", 3);
  EXPECT_EQ(p->SearchHistory("rust", 10).size(), 1u);
  EXPECT_EQ(p->SearchHistory("RELEASED", 10).size(), 1u);
  // LIKE wildcards in the query are literal characters.
  EXPECT_EQ(p->SearchHistory("%", 10).size(), 1u);
  EXPECT_EQ(p->SearchHistory("_", 10).size(), 1u);
  EXPECT_TRUE(p->SearchHistory("nothing", 10).empty());
}

TEST(PlacesHistory, DeletingVisitsUpdatesEntries) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  p->AddVisit("https://a.example/", "A", 100);
  p->AddVisit("https://a.example/", "A", 500);
  p->AddVisit("https://b.example/", "B", 450);
  EXPECT_TRUE(p->DeleteHistoryBetween(400, 600));
  auto a = p->GetHistory("https://a.example/");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->visit_count, 1);
  EXPECT_EQ(a->last_visit, 100);
  EXPECT_FALSE(p->GetHistory("https://b.example/"));  // No visits left.
  EXPECT_TRUE(p->DeleteHistoryUrl("https://a.example/"));
  EXPECT_TRUE(p->RecentHistory(10).empty());
}

TEST(PlacesHistory, ClearedHistoryLeavesNoTraceInTheFiles) {
  ScopedTempDir dir;
  const auto path = dir.path() / "places.db";
  const std::string secret = "https://secret-visit-d41d8cd98f.example/private-page";
  {
    auto p = OpenAt(path);
    ASSERT_TRUE(p);
    ASSERT_TRUE(p->AddVisit(secret, "Secret visit title d41d8cd98f", 1));
    ASSERT_TRUE(p->AddVisit("https://kept.example/", "Kept", 2));
    ASSERT_NE(ReadDatabaseFiles(path).find("d41d8cd98f"), std::string::npos);
    ASSERT_TRUE(p->DeleteHistoryUrl(secret));
    EXPECT_EQ(ReadDatabaseFiles(path).find("d41d8cd98f"), std::string::npos);
    ASSERT_TRUE(p->AddVisit(secret, "Secret visit title d41d8cd98f", 3));
    ASSERT_TRUE(p->ClearHistory());
    EXPECT_EQ(ReadDatabaseFiles(path).find("d41d8cd98f"), std::string::npos);
  }
  EXPECT_EQ(ReadDatabaseFiles(path).find("d41d8cd98f"), std::string::npos);
  EXPECT_EQ(ReadDatabaseFiles(path).find("kept.example"), std::string::npos);
}

TEST(PlacesBookmarks, RootsExistAndStayPut) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  auto bar = p->GetBookmark(kBookmarksBarId);
  ASSERT_TRUE(bar);
  EXPECT_EQ(bar->kind, BookmarkKind::kFolder);
  EXPECT_TRUE(p->GetBookmark(kOtherBookmarksId));
  EXPECT_FALSE(p->RemoveBookmark(kBookmarksBarId));
  EXPECT_FALSE(p->MoveBookmark(kOtherBookmarksId, kBookmarksBarId, 0));
  EXPECT_FALSE(p->UpdateBookmark(kBookmarksBarId, "Renamed", ""));
}

TEST(PlacesBookmarks, AddMoveAndRemoveKeepPositionsContiguous) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  auto a = p->AddBookmark(kBookmarksBarId, "A", "https://a.example/", -1, 1);
  auto b = p->AddBookmark(kBookmarksBarId, "B", "https://b.example/", -1, 2);
  auto c = p->AddBookmark(kBookmarksBarId, "C", "https://c.example/", 0, 3);  // Front.
  ASSERT_TRUE(a && b && c);
  auto order = [&](int64_t folder) {
    std::string s;
    int expected = 0;
    for (const Bookmark& bm : p->Children(folder)) {
      EXPECT_EQ(bm.position, expected++);
      s += bm.title;
    }
    return s;
  };
  EXPECT_EQ(order(kBookmarksBarId), "CAB");
  EXPECT_TRUE(p->MoveBookmark(*c, kBookmarksBarId, 2));
  EXPECT_EQ(order(kBookmarksBarId), "ABC");
  auto folder = p->AddFolder(kBookmarksBarId, "F", 1, 4);
  ASSERT_TRUE(folder);
  EXPECT_EQ(order(kBookmarksBarId), "AFBC");
  EXPECT_TRUE(p->MoveBookmark(*b, *folder, 0));
  EXPECT_EQ(order(kBookmarksBarId), "AFC");
  EXPECT_EQ(order(*folder), "B");
  EXPECT_TRUE(p->RemoveBookmark(*folder));  // Removes B with it.
  EXPECT_EQ(order(kBookmarksBarId), "AC");
  EXPECT_FALSE(p->GetBookmark(*b));
}

TEST(PlacesBookmarks, FoldersCannotMoveIntoThemselves) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  auto outer = p->AddFolder(kOtherBookmarksId, "Outer", -1, 1);
  auto inner = p->AddFolder(*outer, "Inner", -1, 2);
  ASSERT_TRUE(outer && inner);
  EXPECT_FALSE(p->MoveBookmark(*outer, *outer, 0));
  EXPECT_FALSE(p->MoveBookmark(*outer, *inner, 0));
  auto leaf = p->AddBookmark(*inner, "L", "https://l.example/", -1, 3);
  ASSERT_TRUE(leaf);
  EXPECT_FALSE(p->MoveBookmark(*outer, *leaf, 0));  // Not a folder.
  EXPECT_FALSE(p->AddBookmark(*leaf, "X", "https://x.example/", -1, 4));
  EXPECT_TRUE(p->MoveBookmark(*inner, kBookmarksBarId, 0));
}

TEST(PlacesBookmarks, OnlyWebUrlsCanBeBookmarked) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  EXPECT_FALSE(p->AddBookmark(kBookmarksBarId, "x", "javascript:alert(1)", -1, 1));
  EXPECT_FALSE(p->AddBookmark(kBookmarksBarId, "x", "torfox://settings/", -1, 1));
  auto id = p->AddBookmark(kBookmarksBarId, "ok", "https://ok.example/", -1, 1);
  ASSERT_TRUE(id);
  EXPECT_FALSE(p->UpdateBookmark(*id, "x", "data:text/html,x"));
  EXPECT_EQ(p->GetBookmark(*id)->url, "https://ok.example/");
  EXPECT_TRUE(p->UpdateBookmark(*id, "Renamed", "https://new.example/"));
  EXPECT_EQ(p->BookmarksForUrl("https://new.example/").size(), 1u);
  EXPECT_EQ(p->SearchBookmarks("renamed", 10).size(), 1u);
}

TEST(PlacesSuggest, PrefixMatchesFirstAndNoDuplicates) {
  ScopedTempDir dir;
  auto p = OpenAt(dir.path() / "places.db");
  ASSERT_TRUE(p);
  p->AddVisit("https://www.example.com/", "Example", 1);
  p->AddVisit("https://other.org/about-example", "About", 2);
  p->AddBookmark(kBookmarksBarId, "Example docs", "https://docs.example.com/", -1, 3);
  p->AddBookmark(kBookmarksBarId, "Dup", "https://www.example.com/", -1, 4);
  auto s = p->Suggest("exa", 10);
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0].url, "https://www.example.com/");  // Bookmarked prefix match.
  EXPECT_EQ(s[0].source, Suggestion::Source::kBookmark);
  EXPECT_EQ(s[1].url, "https://docs.example.com/");  // Bookmark, substring.
  EXPECT_EQ(s[2].url, "https://other.org/about-example");
  EXPECT_EQ(p->Suggest("exa", 1).size(), 1u);
  EXPECT_TRUE(p->Suggest("", 10).empty());
  EXPECT_TRUE(p->Suggest("zzz", 10).empty());
}

TEST(PlacesFile, DataPersistsAcrossReopen) {
  ScopedTempDir dir;
  const auto path = dir.path() / "places.db";
  {
    auto p = OpenAt(path);
    ASSERT_TRUE(p);
    p->AddVisit("https://a.example/", "A", 1);
    p->AddBookmark(kBookmarksBarId, "A", "https://a.example/", -1, 1);
  }
  Places::OpenStatus status;
  auto p = OpenAt(path, &status);
  ASSERT_TRUE(p);
  EXPECT_EQ(status, Places::OpenStatus::kOpened);
  EXPECT_EQ(p->RecentHistory(10).size(), 1u);
  EXPECT_EQ(p->Children(kBookmarksBarId).size(), 1u);
}

TEST(PlacesFile, CorruptFileIsKeptAsideAndReplaced) {
  ScopedTempDir dir;
  const auto path = dir.path() / "places.db";
  {
    std::ofstream out(path, std::ios::binary);
    out << "this is not a database, but it is the user's file";
  }
  Places::OpenStatus status;
  auto p = OpenAt(path, &status);
  ASSERT_TRUE(p);
  EXPECT_EQ(status, Places::OpenStatus::kRecovered);
  EXPECT_TRUE(p->AddVisit("https://a.example/", "A", 1));
  std::filesystem::path backup = path;
  backup += ".corrupt-1000";
  ASSERT_TRUE(std::filesystem::exists(backup));
  EXPECT_EQ(ReadAll(backup), "this is not a database, but it is the user's file");
}

// Runs |sql| on the database at |path| with a separate connection.
void ExecRaw(const std::filesystem::path& path, const char* sql) {
  sqlite3* db = nullptr;
  const std::u8string u8 = path.u8string();
  ASSERT_EQ(sqlite3_open(reinterpret_cast<const char*>(u8.c_str()), &db), SQLITE_OK);
  EXPECT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
  sqlite3_close(db);
}

TEST(PlacesFile, NewerAndForeignDatabasesAreSetAsideNotModified) {
  for (const char* setup : {
           // A database from a future TorFox version.
           "PRAGMA user_version = 9",
           // Somebody else's database at TorFox's path.
           "DROP TABLE visits; DROP TABLE urls; DROP TABLE bookmarks; "
           "CREATE TABLE notes(text); INSERT INTO notes VALUES ('keep me'); PRAGMA user_version = 0",
       }) {
    ScopedTempDir dir;
    const auto path = dir.path() / "places.db";
    {
      auto p = OpenAt(path);
      ASSERT_TRUE(p);
      p->AddVisit("https://a.example/", "A", 1);
    }
    ExecRaw(path, setup);
    const std::string before = ReadDatabaseFiles(path);
    Places::OpenStatus status;
    auto p = OpenAt(path, &status);
    ASSERT_TRUE(p) << setup;
    EXPECT_EQ(status, Places::OpenStatus::kRecovered) << setup;
    EXPECT_TRUE(p->RecentHistory(10).empty());
    std::filesystem::path backup = path;
    backup += ".corrupt-1000";
    ASSERT_TRUE(std::filesystem::exists(backup)) << setup;
    EXPECT_EQ(ReadAll(backup), before.substr(0, ReadAll(backup).size())) << setup;
  }
}

}  // namespace
}  // namespace torfox::browsing
