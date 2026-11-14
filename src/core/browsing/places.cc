// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/places.h"

#include <system_error>
#include <utility>

#include "sqlite3.h"

namespace torfox::browsing {

namespace {

constexpr int kSchemaVersion = 1;
constexpr int kMaxFolderDepth = 1000;  // Guards walks over a damaged tree.

constexpr char kSchema[] = R"sql(
CREATE TABLE urls(
  id INTEGER PRIMARY KEY,
  url TEXT NOT NULL UNIQUE,
  title TEXT NOT NULL DEFAULT '',
  visit_count INTEGER NOT NULL DEFAULT 0,
  last_visit INTEGER NOT NULL DEFAULT 0);
CREATE INDEX urls_last_visit ON urls(last_visit);
CREATE TABLE visits(
  id INTEGER PRIMARY KEY,
  url_id INTEGER NOT NULL REFERENCES urls(id) ON DELETE CASCADE,
  time INTEGER NOT NULL);
CREATE INDEX visits_time ON visits(time);
CREATE INDEX visits_url ON visits(url_id);
CREATE TABLE bookmarks(
  id INTEGER PRIMARY KEY,
  parent INTEGER REFERENCES bookmarks(id) ON DELETE CASCADE,
  position INTEGER NOT NULL,
  kind INTEGER NOT NULL CHECK (kind IN (0, 1)),
  title TEXT NOT NULL,
  url TEXT,
  added INTEGER NOT NULL);
CREATE INDEX bookmarks_parent ON bookmarks(parent, position);
CREATE INDEX bookmarks_url ON bookmarks(url);
INSERT INTO bookmarks(id, parent, position, kind, title, url, added)
  VALUES (1, NULL, 0, 0, 'Bookmarks bar', NULL, 0), (2, NULL, 1, 0, 'Other bookmarks', NULL, 0);
PRAGMA user_version = 1;
)sql";

// One prepared statement; every failure is sticky and reported by ok().
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    ok_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& Bind(int index, int64_t value) {
    ok_ = ok_ && sqlite3_bind_int64(stmt_, index, value) == SQLITE_OK;
    return *this;
  }
  Stmt& Bind(int index, std::string_view value) {
    // An empty view may have a null data pointer, which SQLite binds as NULL.
    const char* data = value.data() ? value.data() : "";
    ok_ = ok_ && sqlite3_bind_text(stmt_, index, data, static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT) == SQLITE_OK;
    return *this;
  }
  Stmt& BindNull(int index) {
    ok_ = ok_ && sqlite3_bind_null(stmt_, index) == SQLITE_OK;
    return *this;
  }
  // True while a row is available.
  bool Next() {
    if (!ok_) {
      return false;
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    ok_ = rc == SQLITE_DONE;
    return false;
  }
  // Runs a statement that returns no rows.
  bool Run() {
    if (!ok_) {
      return false;
    }
    ok_ = sqlite3_step(stmt_) == SQLITE_DONE;
    return ok_;
  }
  bool ok() const { return ok_; }
  int64_t Int(int column) { return sqlite3_column_int64(stmt_, column); }
  std::string Text(int column) {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size))
                : std::string();
  }

 private:
  sqlite3_stmt* stmt_ = nullptr;
  bool ok_ = false;
};

// Rolls back unless committed.
class Transaction {
 public:
  explicit Transaction(sqlite3* db) : db_(db) {
    active_ = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
  }
  ~Transaction() {
    if (active_) {
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }
  bool ok() const { return active_; }
  bool Commit() {
    if (!active_) {
      return false;
    }
    active_ = false;
    return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
  }

 private:
  sqlite3* db_;
  bool active_ = false;
};

char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (LowerAscii(s[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Valid UTF-8 with control characters replaced by spaces, cut to at most
// kMaxTitleLength bytes on a character boundary. Invalid bytes become U+FFFD.
std::string CleanTitle(std::string_view in) {
  std::string out;
  out.reserve(in.size() < kMaxTitleLength ? in.size() : kMaxTitleLength);
  std::size_t i = 0;
  while (i < in.size()) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                                 : (c & 0xF8) == 0xF0     ? 4
                                                          : 0;
    bool valid = len != 0 && i + len <= in.size();
    for (std::size_t k = 1; valid && k < len; ++k) {
      valid = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
    }
    std::string_view piece;
    if (!valid) {
      piece = "\xEF\xBF\xBD";
      len = 1;
    } else if (c < 0x20 || c == 0x7F) {
      piece = " ";
    } else {
      piece = in.substr(i, len);
    }
    if (out.size() + piece.size() > kMaxTitleLength) {
      break;
    }
    out.append(piece);
    i += len;
  }
  return out;
}

// A LIKE pattern matching |text| literally, with |prefix| and |suffix|.
std::string LikePattern(std::string_view prefix, std::string_view text, std::string_view suffix) {
  std::string out(prefix);
  for (char c : text) {
    if (c == '\\' || c == '%' || c == '_') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.append(suffix);
  return out;
}

std::string Utf8Path(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

HistoryEntry ReadHistory(Stmt& s) {
  HistoryEntry e;
  e.url = s.Text(0);
  e.title = s.Text(1);
  e.visit_count = static_cast<int>(s.Int(2));
  e.last_visit = s.Int(3);
  return e;
}

Bookmark ReadBookmark(Stmt& s) {
  Bookmark b;
  b.id = s.Int(0);
  b.parent = s.Int(1);
  b.position = static_cast<int>(s.Int(2));
  b.kind = s.Int(3) == 0 ? BookmarkKind::kFolder : BookmarkKind::kUrl;
  b.title = s.Text(4);
  b.url = s.Text(5);
  b.added = s.Int(6);
  return b;
}

constexpr char kBookmarkColumns[] = "id, coalesce(parent, 0), position, kind, title, coalesce(url, ''), added";

// Opens |path| and checks it is a healthy TorFox database (or an empty new
// file, which it initialises). Returns nullptr, with |why| set, if not.
sqlite3* OpenChecked(const std::filesystem::path& path, std::string* why) {
  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW;
  const int rc = sqlite3_open_v2(Utf8Path(path).c_str(), &db, flags, nullptr);
  // close_v2 defers the close until every statement is finalized, so an
  // early return never leaves the file open (and locked) behind us.
  auto fail = [&db, why](std::string reason) -> sqlite3* {
    *why = std::move(reason);
    if (db) {
      *why += std::string(" (") + sqlite3_errmsg(db) + ")";
    }
    sqlite3_close_v2(db);
    db = nullptr;
    return nullptr;
  };
  if (rc != SQLITE_OK) {
    return fail("open failed");
  }
  {
    Stmt check(db, "PRAGMA quick_check");
    if (!check.Next() || check.Text(0) != "ok") {
      return fail("integrity check failed");
    }
  }
  int64_t version = -1;
  {
    Stmt v(db, "PRAGMA user_version");
    if (!v.Next()) {
      return fail("no schema version");
    }
    version = v.Int(0);
  }
  if (version == 0) {
    {
      Stmt tables(db, "SELECT count(*) FROM sqlite_master");
      if (!tables.Next() || tables.Int(0) != 0) {
        return fail("not a TorFox database");
      }
    }
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
      return fail("could not create tables");
    }
  } else if (version != kSchemaVersion) {
    return fail("unsupported schema version " + std::to_string(version));
  }
  {
    Stmt roots(db, "SELECT count(*) FROM bookmarks WHERE id IN (1, 2) AND parent IS NULL AND kind = 0");
    if (!roots.Next() || roots.Int(0) != 2) {
      return fail("bookmark folders missing");
    }
  }
  for (const char* pragma : {"PRAGMA foreign_keys = ON", "PRAGMA secure_delete = ON",
                             "PRAGMA journal_mode = WAL", "PRAGMA synchronous = NORMAL",
                             "PRAGMA temp_store = MEMORY"}) {
    if (sqlite3_exec(db, pragma, nullptr, nullptr, nullptr) != SQLITE_OK) {
      return fail(std::string("could not set ") + pragma);
    }
  }
  return db;
}

}  // namespace

bool IsStorableUrl(std::string_view url) {
  if (url.size() > kMaxUrlLength) {
    return false;
  }
  std::size_t prefix = 0;
  if (StartsWithNoCase(url, "https://")) {
    prefix = 8;
  } else if (StartsWithNoCase(url, "http://")) {
    prefix = 7;
  } else {
    return false;
  }
  if (url.size() == prefix || url[prefix] == '/') {
    return false;  // No host.
  }
  for (char c : url) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7F) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<Places> Places::Open(const std::filesystem::path& path,
                                     int64_t now,
                                     OpenStatus* status,
                                     std::string* error) {
  *status = OpenStatus::kFailed;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::string why;
  if (sqlite3* db = OpenChecked(path, &why)) {
    *status = OpenStatus::kOpened;
    return std::unique_ptr<Places>(new Places(db));
  }
  // Keep the unusable file (and its journal) for the user; start afresh.
  const std::string suffix = ".corrupt-" + std::to_string(now);
  for (const char* extra : {"", "-wal", "-shm", "-journal"}) {
    std::filesystem::path from = path;
    from += extra;
    if (std::filesystem::exists(from, ec)) {
      std::filesystem::path to = path;
      to += suffix;
      to += extra;
      std::filesystem::rename(from, to, ec);
      if (ec) {
        *error = why + "; could not move the damaged database aside: " + ec.message();
        return nullptr;
      }
    }
  }
  if (sqlite3* db = OpenChecked(path, &why)) {
    *status = OpenStatus::kRecovered;
    *error = why;  // Why the old file was set aside.
    return std::unique_ptr<Places>(new Places(db));
  }
  *error = "could not create the history and bookmarks database: " + why;
  return nullptr;
}

Places::Places(sqlite3* db) : db_(db) {}

Places::~Places() {
  sqlite3_close_v2(db_);
}

bool Places::Exec(const char* sql) {
  return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Places::Checkpoint() {
  // Copies the log into the database file and truncates it, so pages that
  // held deleted rows do not survive in the -wal file.
  return Exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

// ----- History ---------------------------------------------------------------

bool Places::AddVisit(std::string_view url, std::string_view title, int64_t time) {
  if (!IsStorableUrl(url)) {
    return false;
  }
  const std::string clean = CleanTitle(title);
  Transaction t(db_);
  Stmt upsert(db_,
              "INSERT INTO urls(url, title, visit_count, last_visit) VALUES (?1, ?2, 1, ?3) "
              "ON CONFLICT(url) DO UPDATE SET visit_count = visit_count + 1, "
              "last_visit = max(last_visit, excluded.last_visit), "
              "title = CASE WHEN excluded.title <> '' THEN excluded.title ELSE title END");
  Stmt visit(db_, "INSERT INTO visits(url_id, time) SELECT id, ?2 FROM urls WHERE url = ?1");
  return t.ok() && upsert.Bind(1, url).Bind(2, std::string_view(clean)).Bind(3, time).Run() &&
         visit.Bind(1, url).Bind(2, time).Run() && t.Commit();
}

bool Places::SetTitle(std::string_view url, std::string_view title) {
  const std::string clean = CleanTitle(title);
  Stmt s(db_, "UPDATE urls SET title = ?2 WHERE url = ?1");
  return s.Bind(1, url).Bind(2, std::string_view(clean)).Run() && sqlite3_changes(db_) > 0;
}

std::vector<HistoryEntry> Places::RecentHistory(std::size_t limit, int64_t before) {
  std::vector<HistoryEntry> out;
  Stmt s(db_,
         "SELECT url, title, visit_count, last_visit FROM urls "
         "WHERE ?1 = 0 OR last_visit < ?1 ORDER BY last_visit DESC, id DESC LIMIT ?2");
  s.Bind(1, before).Bind(2, static_cast<int64_t>(limit));
  while (s.Next()) {
    out.push_back(ReadHistory(s));
  }
  return out;
}

std::vector<HistoryEntry> Places::SearchHistory(std::string_view query, std::size_t limit) {
  std::vector<HistoryEntry> out;
  const std::string pattern = LikePattern("%", query, "%");
  Stmt s(db_,
         "SELECT url, title, visit_count, last_visit FROM urls "
         "WHERE url LIKE ?1 ESCAPE '\\' OR title LIKE ?1 ESCAPE '\\' "
         "ORDER BY last_visit DESC, id DESC LIMIT ?2");
  s.Bind(1, std::string_view(pattern)).Bind(2, static_cast<int64_t>(limit));
  while (s.Next()) {
    out.push_back(ReadHistory(s));
  }
  return out;
}

std::optional<HistoryEntry> Places::GetHistory(std::string_view url) {
  Stmt s(db_, "SELECT url, title, visit_count, last_visit FROM urls WHERE url = ?1");
  s.Bind(1, url);
  if (s.Next()) {
    return ReadHistory(s);
  }
  return std::nullopt;
}

bool Places::DeleteHistoryUrl(std::string_view url) {
  Stmt s(db_, "DELETE FROM urls WHERE url = ?1");
  return s.Bind(1, url).Run() && Checkpoint();
}

bool Places::DeleteHistoryBetween(int64_t from, int64_t to) {
  Transaction t(db_);
  Stmt visits(db_, "DELETE FROM visits WHERE time >= ?1 AND time < ?2");
  Stmt recount(db_,
               "UPDATE urls SET visit_count = (SELECT count(*) FROM visits WHERE url_id = urls.id), "
               "last_visit = coalesce((SELECT max(time) FROM visits WHERE url_id = urls.id), 0)");
  Stmt empty(db_, "DELETE FROM urls WHERE visit_count = 0");
  return t.ok() && visits.Bind(1, from).Bind(2, to).Run() && recount.Run() && empty.Run() &&
         t.Commit() && Checkpoint();
}

bool Places::ClearHistory() {
  Transaction t(db_);
  return t.ok() && Exec("DELETE FROM visits") && Exec("DELETE FROM urls") && t.Commit() &&
         Checkpoint();
}

// ----- Bookmarks -------------------------------------------------------------

std::optional<Bookmark> Places::GetBookmark(int64_t id) {
  const std::string sql = std::string("SELECT ") + kBookmarkColumns + " FROM bookmarks WHERE id = ?1";
  Stmt s(db_, sql.c_str());
  s.Bind(1, id);
  if (s.Next()) {
    return ReadBookmark(s);
  }
  return std::nullopt;
}

std::vector<Bookmark> Places::Children(int64_t parent) {
  std::vector<Bookmark> out;
  const std::string sql = std::string("SELECT ") + kBookmarkColumns +
                          " FROM bookmarks WHERE parent = ?1 ORDER BY position, id";
  Stmt s(db_, sql.c_str());
  s.Bind(1, parent);
  while (s.Next()) {
    out.push_back(ReadBookmark(s));
  }
  return out;
}

std::vector<Bookmark> Places::BookmarksForUrl(std::string_view url) {
  std::vector<Bookmark> out;
  const std::string sql = std::string("SELECT ") + kBookmarkColumns +
                          " FROM bookmarks WHERE kind = 1 AND url = ?1 ORDER BY id";
  Stmt s(db_, sql.c_str());
  s.Bind(1, url);
  while (s.Next()) {
    out.push_back(ReadBookmark(s));
  }
  return out;
}

std::vector<Bookmark> Places::SearchBookmarks(std::string_view query, std::size_t limit) {
  std::vector<Bookmark> out;
  const std::string pattern = LikePattern("%", query, "%");
  const std::string sql = std::string("SELECT ") + kBookmarkColumns +
                          " FROM bookmarks WHERE kind = 1 AND (title LIKE ?1 ESCAPE '\\' OR "
                          "url LIKE ?1 ESCAPE '\\') ORDER BY added DESC, id DESC LIMIT ?2";
  Stmt s(db_, sql.c_str());
  s.Bind(1, std::string_view(pattern)).Bind(2, static_cast<int64_t>(limit));
  while (s.Next()) {
    out.push_back(ReadBookmark(s));
  }
  return out;
}

int Places::ChildCount(int64_t parent) {
  Stmt s(db_, "SELECT count(*) FROM bookmarks WHERE parent = ?1");
  s.Bind(1, parent);
  return s.Next() ? static_cast<int>(s.Int(0)) : -1;
}

bool Places::IsDescendantOrSelf(int64_t candidate, int64_t ancestor) {
  int64_t current = candidate;
  for (int depth = 0; current != 0 && depth < kMaxFolderDepth; ++depth) {
    if (current == ancestor) {
      return true;
    }
    auto b = GetBookmark(current);
    if (!b) {
      return false;
    }
    current = b->parent;
  }
  return current != 0;  // Too deep: treat as a cycle and refuse.
}

namespace {

bool IsRoot(int64_t id) {
  return id == kBookmarksBarId || id == kOtherBookmarksId;
}

}  // namespace

std::optional<int64_t> Places::AddBookmark(int64_t parent,
                                           std::string_view title,
                                           std::string_view url,
                                           int position,
                                           int64_t now) {
  if (!IsStorableUrl(url)) {
    return std::nullopt;
  }
  auto folder = GetBookmark(parent);
  if (!folder || folder->kind != BookmarkKind::kFolder) {
    return std::nullopt;
  }
  const std::string clean = CleanTitle(title);
  Transaction t(db_);
  const int count = ChildCount(parent);
  if (!t.ok() || count < 0) {
    return std::nullopt;
  }
  const int pos = position < 0 || position > count ? count : position;
  Stmt shift(db_, "UPDATE bookmarks SET position = position + 1 WHERE parent = ?1 AND position >= ?2");
  Stmt insert(db_,
              "INSERT INTO bookmarks(parent, position, kind, title, url, added) "
              "VALUES (?1, ?2, 1, ?3, ?4, ?5)");
  if (!shift.Bind(1, parent).Bind(2, static_cast<int64_t>(pos)).Run() ||
      !insert.Bind(1, parent)
           .Bind(2, static_cast<int64_t>(pos))
           .Bind(3, std::string_view(clean))
           .Bind(4, url)
           .Bind(5, now)
           .Run()) {
    return std::nullopt;
  }
  const int64_t id = sqlite3_last_insert_rowid(db_);
  if (!t.Commit()) {
    return std::nullopt;
  }
  return id;
}

std::optional<int64_t> Places::AddFolder(int64_t parent, std::string_view title, int position,
                                         int64_t now) {
  auto folder = GetBookmark(parent);
  if (!folder || folder->kind != BookmarkKind::kFolder) {
    return std::nullopt;
  }
  const std::string clean = CleanTitle(title);
  Transaction t(db_);
  const int count = ChildCount(parent);
  if (!t.ok() || count < 0) {
    return std::nullopt;
  }
  const int pos = position < 0 || position > count ? count : position;
  Stmt shift(db_, "UPDATE bookmarks SET position = position + 1 WHERE parent = ?1 AND position >= ?2");
  Stmt insert(db_,
              "INSERT INTO bookmarks(parent, position, kind, title, url, added) "
              "VALUES (?1, ?2, 0, ?3, NULL, ?4)");
  if (!shift.Bind(1, parent).Bind(2, static_cast<int64_t>(pos)).Run() ||
      !insert.Bind(1, parent).Bind(2, static_cast<int64_t>(pos)).Bind(3, std::string_view(clean)).Bind(4, now).Run()) {
    return std::nullopt;
  }
  const int64_t id = sqlite3_last_insert_rowid(db_);
  if (!t.Commit()) {
    return std::nullopt;
  }
  return id;
}

bool Places::UpdateBookmark(int64_t id, std::string_view title, std::string_view url) {
  auto b = GetBookmark(id);
  if (!b || IsRoot(id)) {
    return false;
  }
  const std::string clean = CleanTitle(title);
  if (b->kind == BookmarkKind::kFolder) {
    Stmt s(db_, "UPDATE bookmarks SET title = ?2 WHERE id = ?1");
    return s.Bind(1, id).Bind(2, std::string_view(clean)).Run();
  }
  if (!IsStorableUrl(url)) {
    return false;
  }
  Stmt s(db_, "UPDATE bookmarks SET title = ?2, url = ?3 WHERE id = ?1");
  return s.Bind(1, id).Bind(2, std::string_view(clean)).Bind(3, url).Run();
}

bool Places::MoveBookmark(int64_t id, int64_t parent, int position) {
  auto b = GetBookmark(id);
  auto folder = GetBookmark(parent);
  if (!b || IsRoot(id) || !folder || folder->kind != BookmarkKind::kFolder ||
      IsDescendantOrSelf(parent, id)) {
    return false;
  }
  Transaction t(db_);
  Stmt close_gap(db_, "UPDATE bookmarks SET position = position - 1 WHERE parent = ?1 AND position > ?2");
  Stmt detach(db_, "UPDATE bookmarks SET parent = NULL, position = -1 WHERE id = ?1");
  if (!t.ok() || !close_gap.Bind(1, b->parent).Bind(2, static_cast<int64_t>(b->position)).Run() ||
      !detach.Bind(1, id).Run()) {
    return false;
  }
  const int count = ChildCount(parent);
  if (count < 0) {
    return false;
  }
  const int pos = position < 0 || position > count ? count : position;
  Stmt open_gap(db_, "UPDATE bookmarks SET position = position + 1 WHERE parent = ?1 AND position >= ?2");
  Stmt attach(db_, "UPDATE bookmarks SET parent = ?2, position = ?3 WHERE id = ?1");
  return open_gap.Bind(1, parent).Bind(2, static_cast<int64_t>(pos)).Run() &&
         attach.Bind(1, id).Bind(2, parent).Bind(3, static_cast<int64_t>(pos)).Run() && t.Commit();
}

bool Places::RemoveBookmark(int64_t id) {
  auto b = GetBookmark(id);
  if (!b || IsRoot(id)) {
    return false;
  }
  Transaction t(db_);
  Stmt remove(db_, "DELETE FROM bookmarks WHERE id = ?1");
  Stmt close_gap(db_, "UPDATE bookmarks SET position = position - 1 WHERE parent = ?1 AND position > ?2");
  return t.ok() && remove.Bind(1, id).Run() &&
         close_gap.Bind(1, b->parent).Bind(2, static_cast<int64_t>(b->position)).Run() &&
         t.Commit() && Checkpoint();
}

// ----- Address bar -----------------------------------------------------------

std::vector<Suggestion> Places::Suggest(std::string_view text, std::size_t limit) {
  std::vector<Suggestion> out;
  if (text.empty() || limit == 0) {
    return out;
  }
  auto add = [&](Suggestion::Source source, std::string url, std::string title) {
    if (out.size() >= limit) {
      return;
    }
    for (const Suggestion& s : out) {
      if (s.url == url) {
        return;
      }
    }
    out.push_back(Suggestion{source, std::move(url), std::move(title)});
  };
  auto collect = [&](Stmt& s, Suggestion::Source source) {
    while (s.Next()) {
      add(source, s.Text(0), s.Text(1));
    }
  };
  const int64_t max = static_cast<int64_t>(limit);

  // 1. The URL starts with |text| once "http(s)://" and "www." are ignored.
  const std::string prefixes[] = {
      LikePattern("http://", text, "%"), LikePattern("https://", text, "%"),
      LikePattern("http://www.", text, "%"), LikePattern("https://www.", text, "%")};
  constexpr char kPrefixMatch[] =
      "(url LIKE ?1 ESCAPE '\\' OR url LIKE ?2 ESCAPE '\\' OR url LIKE ?3 ESCAPE '\\' OR "
      "url LIKE ?4 ESCAPE '\\')";
  auto bind_prefixes = [&](Stmt& s) -> Stmt& {
    return s.Bind(1, std::string_view(prefixes[0]))
        .Bind(2, std::string_view(prefixes[1]))
        .Bind(3, std::string_view(prefixes[2]))
        .Bind(4, std::string_view(prefixes[3]))
        .Bind(5, max);
  };
  {
    const std::string sql = std::string("SELECT url, title FROM bookmarks WHERE kind = 1 AND ") +
                            kPrefixMatch + " ORDER BY added DESC LIMIT ?5";
    Stmt s(db_, sql.c_str());
    collect(bind_prefixes(s), Suggestion::Source::kBookmark);
  }
  {
    const std::string sql = std::string("SELECT url, title FROM urls WHERE ") + kPrefixMatch +
                            " ORDER BY visit_count DESC, last_visit DESC LIMIT ?5";
    Stmt s(db_, sql.c_str());
    collect(bind_prefixes(s), Suggestion::Source::kHistory);
  }

  // 2. |text| appears anywhere in the title or URL.
  const std::string anywhere = LikePattern("%", text, "%");
  {
    Stmt s(db_,
           "SELECT url, title FROM bookmarks WHERE kind = 1 AND "
           "(title LIKE ?1 ESCAPE '\\' OR url LIKE ?1 ESCAPE '\\') ORDER BY added DESC LIMIT ?2");
    collect(s.Bind(1, std::string_view(anywhere)).Bind(2, max), Suggestion::Source::kBookmark);
  }
  {
    Stmt s(db_,
           "SELECT url, title FROM urls WHERE (title LIKE ?1 ESCAPE '\\' OR url LIKE ?1 ESCAPE '\\') "
           "ORDER BY visit_count DESC, last_visit DESC LIMIT ?2");
    collect(s.Bind(1, std::string_view(anywhere)).Bind(2, max), Suggestion::Source::kHistory);
  }
  return out;
}

}  // namespace torfox::browsing
