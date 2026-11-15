// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/chromium_profile.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

#include "gtest/gtest.h"
#include "sqlite3.h"

namespace torfox::chromium_profile {
namespace {

class ChromiumProfileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("torfox-profile-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void Sql(const std::string& sql) {
    sqlite3* db = nullptr;
    const std::u8string path = (dir_ / "History").u8string();
    ASSERT_EQ(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << sql;
    sqlite3_close_v2(db);
  }

  int Count(const std::string& table) {
    sqlite3* db = nullptr;
    const std::u8string path = (dir_ / "History").u8string();
    EXPECT_EQ(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db, SQLITE_OPEN_READONLY,
                              nullptr),
              SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, ("SELECT count(*) FROM " + table).c_str(), -1, &stmt, nullptr),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const int n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    return n;
  }

  std::string Bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  std::filesystem::path dir_;
};

TEST_F(ChromiumProfileTest, ErasesDownloadRecordsOnly) {
  Sql("CREATE TABLE downloads (id INTEGER PRIMARY KEY, target_path TEXT);"
      "CREATE TABLE downloads_url_chains (id INTEGER, chain_index INTEGER, url TEXT);"
      "CREATE TABLE downloads_slices (download_id INTEGER, offset INTEGER);"
      "CREATE TABLE urls (id INTEGER PRIMARY KEY, url TEXT);"
      "INSERT INTO downloads VALUES (1, 'C:/secret-target-path/report.pdf');"
      "INSERT INTO downloads_url_chains VALUES (1, 0, 'https://secret-origin.example/report.pdf');"
      "INSERT INTO downloads_slices VALUES (1, 0);"
      "INSERT INTO urls VALUES (1, 'https://kept.example/');");
  { std::ofstream(dir_ / "DownloadMetadata") << "secret-origin.example"; }

  const ScrubResult r = ScrubDownloadRecords(dir_);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows, 3);
  EXPECT_TRUE(r.metadata_removed);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "DownloadMetadata"));
  EXPECT_EQ(Count("downloads"), 0);
  EXPECT_EQ(Count("downloads_url_chains"), 0);
  EXPECT_EQ(Count("downloads_slices"), 0);
  EXPECT_EQ(Count("urls"), 1);  // Not a download record.
  // Overwritten and compacted: nothing readable is left in the file.
  const std::string bytes = Bytes(dir_ / "History");
  EXPECT_EQ(bytes.find("secret-target-path"), std::string::npos);
  EXPECT_EQ(bytes.find("secret-origin"), std::string::npos);
  EXPECT_NE(bytes.find("kept.example"), std::string::npos);

  // Nothing left to do the second time.
  const ScrubResult again = ScrubDownloadRecords(dir_);
  EXPECT_TRUE(again.ok);
  EXPECT_EQ(again.rows, 0);
}

TEST_F(ChromiumProfileTest, MissingProfileIsFine) {
  const ScrubResult r = ScrubDownloadRecords(dir_ / "absent");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows, 0);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "absent"));  // Nothing created.
}

TEST_F(ChromiumProfileTest, UnreadableDatabaseIsReportedAndLeftAlone) {
  { std::ofstream(dir_ / "History", std::ios::binary) << "not a database at all, but long enough to look like one"; }
  const std::string before = Bytes(dir_ / "History");
  const ScrubResult r = ScrubDownloadRecords(dir_);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(Bytes(dir_ / "History"), before);
}

TEST_F(ChromiumProfileTest, ErasesSiteScoresAndOptionallyZoomLevels) {
  const std::string prefs = R"({"browser":{"window_placement":{"left":10}},)"
                            R"("partition":{"per_host_zoom_levels":{"x":{"zoomed.example":{"zoom_level":1.5}}}},)"
                            R"("profile":{"content_settings":{"exceptions":{)"
                            R"("site_engagement":{"https://visited.example:443,*":{"setting":{"rawScore":3.2}}},)"
                            R"("media_engagement":{"https://visited.example:443,*":{"setting":{}}},)"
                            R"("cookies":{"https://kept.example:443,*":{"setting":1}}}},"name":"Person 1"}})";
  std::ofstream(dir_ / "Preferences", std::ios::binary) << prefs;

  SiteRecordsResult r = ScrubSiteRecords(dir_, /*zoom_levels=*/false);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.removed, 2);
  std::string after = Bytes(dir_ / "Preferences");
  EXPECT_EQ(after.find("visited.example"), std::string::npos);
  EXPECT_NE(after.find("zoomed.example"), std::string::npos);
  EXPECT_NE(after.find("kept.example"), std::string::npos);  // Other settings stay.
  EXPECT_NE(after.find("Person 1"), std::string::npos);
  EXPECT_NE(after.find("window_placement"), std::string::npos);

  r = ScrubSiteRecords(dir_, /*zoom_levels=*/true);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.removed, 1);
  after = Bytes(dir_ / "Preferences");
  EXPECT_EQ(after.find("zoomed.example"), std::string::npos);
  EXPECT_NE(after.find("kept.example"), std::string::npos);

  // Nothing left to erase: the file is not rewritten.
  const auto before = std::filesystem::last_write_time(dir_ / "Preferences");
  r = ScrubSiteRecords(dir_, true);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.removed, 0);
  // Compared, not printed: gtest prints a time point with std::format,
  // which macOS before 13.3 lacks for floating point.
  EXPECT_TRUE(std::filesystem::last_write_time(dir_ / "Preferences") == before);
}

TEST_F(ChromiumProfileTest, UnreadablePreferencesAreReportedAndLeftAlone) {
  EXPECT_TRUE(ScrubSiteRecords(dir_ / "absent", true).ok);
  std::ofstream(dir_ / "Preferences", std::ios::binary) << "{ not json";
  const SiteRecordsResult r = ScrubSiteRecords(dir_, true);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(Bytes(dir_ / "Preferences"), "{ not json");
}

TEST_F(ChromiumProfileTest, WritesTheProxyModeKeepingEverythingElse) {
  // A new profile: the folder and file are created.
  const std::filesystem::path fresh = dir_ / "New";
  ASSERT_TRUE(WriteProxyMode(fresh, "direct"));
  EXPECT_EQ(Bytes(fresh / "Preferences"), R"({"proxy":{"mode":"direct"}})");

  std::ofstream(dir_ / "Preferences", std::ios::binary)
      << R"({"proxy":{"mode":"system","server":"x"},"profile":{"name":"Person 1"}})";
  ASSERT_TRUE(WriteProxyMode(dir_, "direct"));
  std::string after = Bytes(dir_ / "Preferences");
  EXPECT_NE(after.find(R"("proxy":{"mode":"direct"})"), std::string::npos);
  EXPECT_NE(after.find("Person 1"), std::string::npos);
  ASSERT_TRUE(WriteProxyMode(dir_, "system"));
  EXPECT_NE(Bytes(dir_ / "Preferences").find(R"("proxy":{"mode":"system"})"), std::string::npos);

  std::ofstream(dir_ / "Preferences", std::ios::binary) << "{ not json";
  EXPECT_FALSE(WriteProxyMode(dir_, "direct"));
  EXPECT_EQ(Bytes(dir_ / "Preferences"), "{ not json");
}

}  // namespace
}  // namespace torfox::chromium_profile
