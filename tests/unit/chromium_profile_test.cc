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

}  // namespace
}  // namespace torfox::chromium_profile
