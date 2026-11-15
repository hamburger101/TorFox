// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_PLACES_H_
#define TORFOX_CORE_BROWSING_PLACES_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace torfox::browsing {

// History and bookmarks for the Normal profile, in one SQLite database.
// Private windows and Tor tabs never write history here.
//
// Only http(s) URLs are stored: bookmarks can never hold javascript:, data:
// or torfox: URLs. Times are milliseconds since the Unix epoch, supplied by
// the caller. Not thread-safe; TorFox uses it on the browser UI thread.
//
// Deleted rows are overwritten in the file (SQLite secure_delete) and the
// write-ahead log is truncated after deletions, so removed history does not
// linger in TorFox's files. The storage device itself may still keep old
// blocks (SSD wear levelling); see docs/STAGE2_FINDINGS.md.

struct HistoryEntry {
  std::string url;
  std::string title;
  int visit_count = 0;
  int64_t last_visit = 0;
};

enum class BookmarkKind { kFolder = 0, kUrl = 1 };

struct Bookmark {
  int64_t id = 0;
  int64_t parent = 0;  // 0 for the two roots.
  int position = 0;
  BookmarkKind kind = BookmarkKind::kUrl;
  std::string title;
  std::string url;  // Empty for folders.
  int64_t added = 0;
};

struct Suggestion {
  enum class Source { kBookmark, kHistory };
  Source source = Source::kHistory;
  std::string url;
  std::string title;
};

inline constexpr int64_t kBookmarksBarId = 1;
inline constexpr int64_t kOtherBookmarksId = 2;
inline constexpr std::size_t kMaxUrlLength = 8192;
inline constexpr std::size_t kMaxTitleLength = 1024;  // Bytes; longer titles are cut.

// True for URLs Places stores: absolute http(s), at most kMaxUrlLength, no
// whitespace or control characters.
bool IsStorableUrl(std::string_view url);

class Places {
 public:
  enum class OpenStatus {
    kOpened,     // Existing database opened (or a new one created).
    kRecovered,  // The file was unreadable or not TorFox's; it was renamed
                 // to "<name>.corrupt-<time>" and a new database created.
    kFailed,
  };

  // Opens or creates the database at |path|. |now| names a recovered file.
  static std::unique_ptr<Places> Open(const std::filesystem::path& path,
                                      int64_t now,
                                      OpenStatus* status,
                                      std::string* error);
  ~Places();
  Places(const Places&) = delete;
  Places& operator=(const Places&) = delete;

  // ----- History -----
  bool AddVisit(std::string_view url, std::string_view title, int64_t time);
  bool SetTitle(std::string_view url, std::string_view title);
  // Newest first; only entries last visited before |before| (0: no limit).
  std::vector<HistoryEntry> RecentHistory(std::size_t limit, int64_t before = 0);
  // Substring match on URL or title, newest first.
  std::vector<HistoryEntry> SearchHistory(std::string_view query, std::size_t limit);
  std::optional<HistoryEntry> GetHistory(std::string_view url);
  bool DeleteHistoryUrl(std::string_view url);
  // Removes visits in [from, to); URLs left without visits are removed.
  bool DeleteHistoryBetween(int64_t from, int64_t to);
  bool ClearHistory();

  // ----- Bookmarks -----
  // |position| < 0 or past the end appends. Returns the new id.
  std::optional<int64_t> AddBookmark(int64_t parent,
                                     std::string_view title,
                                     std::string_view url,
                                     int position,
                                     int64_t now);
  std::optional<int64_t> AddFolder(int64_t parent, std::string_view title, int position,
                                   int64_t now);
  // |url| is ignored for folders.
  bool UpdateBookmark(int64_t id, std::string_view title, std::string_view url);
  // Moves |id| under |parent| at |position|. Roots cannot move, and a folder
  // cannot move into itself or its descendants.
  bool MoveBookmark(int64_t id, int64_t parent, int position);
  // Removes a bookmark, or a folder with everything in it. Roots stay.
  bool RemoveBookmark(int64_t id);
  std::optional<Bookmark> GetBookmark(int64_t id);
  std::vector<Bookmark> Children(int64_t parent);
  std::vector<Bookmark> BookmarksForUrl(std::string_view url);
  std::vector<Bookmark> SearchBookmarks(std::string_view query, std::size_t limit);

  // ----- Address bar -----
  // Bookmarks and history matching |text|: URL prefix matches (ignoring the
  // scheme and "www.") first, then title and URL substring matches. Without
  // |history|, bookmarks only (Tor windows).
  std::vector<Suggestion> Suggest(std::string_view text, std::size_t limit, bool history = true);

 private:
  explicit Places(sqlite3* db);
  bool Exec(const char* sql);
  bool Checkpoint();
  bool IsDescendantOrSelf(int64_t candidate, int64_t ancestor);
  int ChildCount(int64_t parent);

  sqlite3* db_;
};

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_PLACES_H_
