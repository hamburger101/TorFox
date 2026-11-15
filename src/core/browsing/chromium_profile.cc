// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/chromium_profile.h"

#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include "sqlite3.h"
#include "src/core/json.h"
#include "src/core/util/file_io.h"

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

SiteRecordsResult ScrubSiteRecords(const std::filesystem::path& profile_dir, bool zoom_levels) {
  constexpr std::size_t kMaxBytes = 32u << 20;
  constexpr int kMaxDepth = 200;
  SiteRecordsResult result;
  const std::filesystem::path file = profile_dir / "Preferences";
  const ReadResult read = ReadFileLimited(file, kMaxBytes);
  if (read.status == ReadStatus::kMissing) {
    return result;
  }
  std::optional<Json> prefs;
  if (read.status == ReadStatus::kOk) {
    prefs = ParseJson(read.data, kMaxBytes, kMaxDepth);
  }
  if (!prefs || !prefs->is_object()) {
    result.ok = false;  // Left alone: Chromium resets a file it cannot read.
    return result;
  }
  auto erase = [&](std::initializer_list<const char*> path) {
    Json* node = &*prefs;
    const char* last = nullptr;
    for (const char* key : path) {
      if (last) {
        auto it = node->find(last);
        if (it == node->end() || !it->is_object()) {
          return;
        }
        node = &*it;
      }
      last = key;
    }
    if (node->erase(last) > 0) {
      ++result.removed;
    }
  };
  erase({"profile", "content_settings", "exceptions", "site_engagement"});
  erase({"profile", "content_settings", "exceptions", "media_engagement"});
  if (zoom_levels) {
    erase({"partition", "per_host_zoom_levels"});
  }
  if (result.removed > 0 && !WriteFileAtomically(file, DumpJson(*prefs))) {
    result.ok = false;
  }
  return result;
}

bool WriteProxyMode(const std::filesystem::path& profile_dir, std::string_view mode) {
  constexpr std::size_t kMaxBytes = 32u << 20;
  const std::filesystem::path file = profile_dir / "Preferences";
  const ReadResult read = ReadFileLimited(file, kMaxBytes);
  Json prefs = Json::object();
  if (read.status == ReadStatus::kOk) {
    std::optional<Json> parsed = ParseJson(read.data, kMaxBytes, 200);
    if (!parsed || !parsed->is_object()) {
      return false;
    }
    prefs = std::move(*parsed);
  } else if (read.status != ReadStatus::kMissing) {
    return false;
  }
  Json proxy = Json::object();
  proxy["mode"] = std::string(mode);
  if (const auto it = prefs.find("proxy"); it != prefs.end() && *it == proxy) {
    return true;
  }
  prefs["proxy"] = std::move(proxy);
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  return !ec && WriteFileAtomically(file, DumpJson(prefs));
}

}  // namespace torfox::chromium_profile
