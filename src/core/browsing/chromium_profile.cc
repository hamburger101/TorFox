// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/chromium_profile.h"

#include <string>
#include <system_error>
#include <vector>

#include "sqlite3.h"

namespace torfox::chromium_profile {

namespace {

std::string Utf8Path(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

bool Exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// Every table holding download records: "downloads", "downloads_url_chains",
// "downloads_slices" and any Chromium adds under the same prefix.
std::vector<std::string> DownloadTables(sqlite3* db, bool* ok) {
  std::vector<std::string> out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT name FROM sqlite_master WHERE type = 'table' AND "
                         "(name = 'downloads' OR substr(name, 1, 10) = 'downloads_')",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    *ok = false;
    return out;
  }
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(stmt, 0);
    if (name) {
      out.emplace_back(reinterpret_cast<const char*>(name));
    }
  }
  if (rc != SQLITE_DONE) {
    *ok = false;
  }
  sqlite3_finalize(stmt);
  return out;
}

std::string Quoted(const std::string& name) {
  std::string out = "\"";
  for (char c : name) {
    out += c;
    if (c == '"') {
      out += '"';
    }
  }
  return out + "\"";
}

}  // namespace

ScrubResult ScrubDownloadRecords(const std::filesystem::path& profile_dir) {
  ScrubResult result;
  std::error_code ec;
  const std::filesystem::path metadata = profile_dir / "DownloadMetadata";
  if (std::filesystem::is_regular_file(metadata, ec)) {
    result.metadata_removed = std::filesystem::remove(metadata, ec);
    if (!result.metadata_removed) {
      result.ok = false;
    }
  }

  const std::filesystem::path history = profile_dir / "History";
  if (!std::filesystem::is_regular_file(history, ec)) {
    return result;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(Utf8Path(history).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    result.ok = false;
    return result;
  }
  sqlite3_busy_timeout(db, 2000);
  bool ok = Exec(db, "PRAGMA secure_delete = ON") && Exec(db, "BEGIN IMMEDIATE");
  if (ok) {
    for (const std::string& table : DownloadTables(db, &ok)) {
      if (!ok || !Exec(db, ("DELETE FROM " + Quoted(table)).c_str())) {
        ok = false;
        break;
      }
      result.rows += sqlite3_changes(db);
    }
    ok = ok && Exec(db, "COMMIT");
    if (!ok) {
      Exec(db, "ROLLBACK");
    }
  }
  if (ok && result.rows > 0) {
    // Freed pages were overwritten already; this also gives them back.
    ok = Exec(db, "VACUUM");
  }
  sqlite3_close_v2(db);
  result.ok = result.ok && ok;
  return result;
}

}  // namespace torfox::chromium_profile
