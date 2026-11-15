// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/places_service.h"

#include <chrono>
#include <vector>

#include "src/browser/app/test_log.h"

namespace torfox {

PlacesService::PlacesService(std::filesystem::path file) : file_(std::move(file)) {}

PlacesService::~PlacesService() = default;

int64_t PlacesService::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void PlacesService::Open() {
  browsing::Places::OpenStatus status = browsing::Places::OpenStatus::kFailed;
  std::string error;
  places_ = browsing::Places::Open(file_, NowMs(), &status, &error);
  const char* name = status == browsing::Places::OpenStatus::kOpened      ? "opened"
                     : status == browsing::Places::OpenStatus::kRecovered ? "recovered"
                                                                          : "failed";
  test_log::Event("places_open", {{"status", name}, {"error", error}});
  if (!places_) {
    notice_ = "History and bookmarks are unavailable: " + error;
  } else if (status == browsing::Places::OpenStatus::kRecovered) {
    notice_ = "TorFox found a damaged history and bookmarks file, set it aside and started a "
              "new one. " + error;
  }
}

int PlacesService::AddObserver(Observer observer) {
  const int id = next_observer_id_++;
  observers_[id] = std::move(observer);
  return id;
}

void PlacesService::RemoveObserver(int id) {
  observers_.erase(id);
}

void PlacesService::Notify(Change change) {
  // Copy: an observer may remove itself.
  std::vector<Observer> observers;
  for (const auto& [id, o] : observers_) {
    observers.push_back(o);
  }
  for (const Observer& o : observers) {
    o(change);
  }
}

Json PlacesService::BookmarkJson(const browsing::Bookmark& b) {
  return {{"id", b.id},
          {"parent", b.parent},
          {"index", b.position},
          {"folder", b.kind == browsing::BookmarkKind::kFolder},
          {"title", b.title},
          {"url", b.url},
          {"added", b.added}};
}

Json PlacesService::HistoryJson(const browsing::HistoryEntry& e) {
  return {{"url", e.url}, {"title", e.title}, {"visits", e.visit_count}, {"lastVisit", e.last_visit}};
}

}  // namespace torfox
