// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_PLACES_SERVICE_H_
#define TORFOX_BROWSER_APP_PLACES_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "src/core/browsing/places.h"
#include "src/core/json.h"

namespace torfox {

// Owns the history and bookmarks database (<user data>/places.db) on the UI
// thread and tells windows and pages when it changes.
//
// If the database cannot be opened TorFox keeps running without history or
// bookmarks and says so on those pages; nothing else depends on it. A damaged
// file is set aside (never deleted) and the user is told where it went.
class PlacesService {
 public:
  enum class Change { kHistory, kBookmarks };
  using Observer = std::function<void(Change)>;

  explicit PlacesService(std::filesystem::path file);
  ~PlacesService();

  void Open();
  // Null when the database is unavailable.
  browsing::Places* places() { return places_.get(); }
  // A message for the user if the database was unavailable or recovered.
  const std::string& notice() const { return notice_; }

  int AddObserver(Observer observer);
  void RemoveObserver(int id);
  void Notify(Change change);

  static int64_t NowMs();

  // JSON for the UI.
  static Json BookmarkJson(const browsing::Bookmark& b);
  static Json HistoryJson(const browsing::HistoryEntry& e);

 private:
  std::filesystem::path file_;
  std::unique_ptr<browsing::Places> places_;
  std::string notice_;
  std::map<int, Observer> observers_;
  int next_observer_id_ = 1;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_PLACES_SERVICE_H_
