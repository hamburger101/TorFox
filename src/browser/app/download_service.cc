// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/download_service.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/places_service.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"

namespace torfox {

namespace {

bool Finished(DownloadService::State s) {
  return s == DownloadService::State::kComplete || s == DownloadService::State::kCancelled ||
         s == DownloadService::State::kFailed;
}

// Equal paths, ignoring the case of ASCII letters.
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
  const auto& x = a.native();
  const auto& y = b.native();
  if (x.size() != y.size()) {
    return false;
  }
  for (std::size_t i = 0; i < x.size(); ++i) {
    auto c = x[i];
    auto d = y[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<decltype(c)>(c + 32);
    }
    if (d >= 'A' && d <= 'Z') {
      d = static_cast<decltype(d)>(d + 32);
    }
    if (c != d) {
      return false;
    }
  }
  return true;
}

}  // namespace

DownloadService::DownloadService(SettingsService* settings, std::filesystem::path dir_override,
                                 std::filesystem::path staging_dir)
    : settings_(settings),
      dir_override_(std::move(dir_override)),
      staging_dir_(std::move(staging_dir)) {
  if (settings_) {
    settings_observer_ = settings_->AddObserver([this](const std::string& key) {
      if (key == downloads::kDisableSetting && Decide() == downloads::Decision::kBlock) {
        CancelAll("downloads disabled");
      }
    });
  }
}

DownloadService::~DownloadService() {
  if (settings_ && settings_observer_) {
    settings_->RemoveObserver(settings_observer_);
  }
}

downloads::Decision DownloadService::Decide() const {
  if (!settings_) {
    return downloads::Decision::kBlock;
  }
  return downloads::Decide(settings_->store().GetBool(downloads::kDisableSetting),
                           settings_->store().GetBool(downloads::kAskSetting));
}

std::filesystem::path DownloadService::Directory() const {
  return dir_override_.empty() ? platform::DownloadsDir() : dir_override_;
}

bool DownloadService::PrepareStaging() {
  staging_ready_ = false;
  if (staging_dir_.empty()) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(staging_dir_, ec);
  if (!std::filesystem::is_directory(staging_dir_, ec)) {
    test_log::Event("download_staging", {{"ok", false}, {"removed", 0}});
    return false;
  }
  int removed = 0;
  int failed = 0;
  for (std::filesystem::directory_iterator it(staging_dir_, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code e;
    // Files only; TorFox never creates folders here, and never follows links.
    if (it->is_symlink(e) || it->is_regular_file(e)) {
      if (std::filesystem::remove(it->path(), e)) {
        ++removed;
      } else {
        ++failed;
      }
    }
  }
  staging_ready_ = !ec;
  test_log::Event("download_staging",
                  {{"ok", staging_ready_}, {"removed", removed}, {"failed", failed}});
  return staging_ready_;
}

std::filesystem::path DownloadService::PartialPath(const std::filesystem::path& final_path) {
  std::filesystem::path p = final_path;
  p += std::string(downloads::kPartialSuffix);
  return p;
}

bool DownloadService::PathInUse(const std::filesystem::path& path) const {
  const std::filesystem::path partial = PartialPath(path);
  for (const Entry& e : entries_) {
    if (Finished(e.state)) {
      continue;
    }
    for (const std::filesystem::path* used : {&e.path, &e.partial}) {
      if (!used->empty() && (SamePath(*used, path) || SamePath(*used, partial))) {
        return true;
      }
    }
  }
  return false;
}


DownloadService::Entry* DownloadService::Find(uint32_t id) {
  for (Entry& e : entries_) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

DownloadService::Entry* DownloadService::FindItem(int context, uint32_t item_id) {
  for (Entry& e : entries_) {
    if (e.context == context && e.item_id == item_id) {
      return &e;
    }
  }
  return nullptr;
}

DownloadService::Entry& DownloadService::Add(Entry entry) {
  if (entry.url.size() > 2048) {
    entry.url.resize(2048);
  }
  entry.id = ++next_id_;
  entry.started_ms = PlacesService::NowMs();
  // Chromium never reuses an id within a profile and session, but never
  // keep two entries for one download.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return e.context == entry.context && e.item_id == entry.item_id;
                                }),
                 entries_.end());
  // A long session keeps at most 500 entries; the oldest finished go first.
  while (entries_.size() >= 500) {
    auto old = std::find_if(entries_.begin(), entries_.end(),
                            [](const Entry& e) { return Finished(e.state); });
    if (old == entries_.end()) {
      break;
    }
    entries_.erase(old);
  }
  entries_.push_back(std::move(entry));
  return entries_.back();
}

DownloadService::Entry* DownloadService::Update(int context,
                                                CefRefPtr<CefDownloadItem> item,
                                                CefRefPtr<CefDownloadItemCallback> control) {
  CEF_REQUIRE_UI_THREAD();
  if (!item || !item->IsValid()) {
    return nullptr;
  }
  Entry* e = FindItem(context, item->GetId());
  if (!e) {
    // Updates may arrive before OnBeforeDownload. A download TorFox has not
    // accepted yet is left alone while it waits for that decision, unless
    // downloads are disabled now.
    if (Decide() == downloads::Decision::kBlock && control && item->IsInProgress()) {
      control->Cancel();
      test_log::Event("download_cancelled", {{"why", "downloads disabled"}, {"known", false}});
    }
    return nullptr;
  }
  e->control = control;
  e->received = item->GetReceivedBytes();
  e->total = item->GetTotalBytes();
  if (Finished(e->state) || e->finishing) {
    // Already decided; later updates change nothing. TorFox never resumes a
    // download: if Chromium resumes one by itself (not seen with CEF 156,
    // see docs/STAGE3_FINDINGS.md), it is cancelled again.
    if (!e->finishing && e->state != State::kComplete && control && item->IsInProgress()) {
      control->Cancel();
      if (e->state == State::kFailed) {
        test_log::Event("download_resume_refused", {{"id", e->id}});
      }
    }
    Notify();
    return e;
  }
  if (item->IsComplete()) {
    e->finishing = true;
    const uint32_t id = e->id;
    FinishFile(id, 0);
    return Find(id);
  } else if (item->IsCanceled()) {
    e->state = State::kCancelled;
    LogFinished(*e, static_cast<int>(item->GetInterruptReason()));
  } else if (item->IsInterrupted()) {
    // TorFox does not resume downloads. Chromium keeps the partial file of
    // an interrupted download for a resume, and CEF's Cancel() does nothing
    // once a download is interrupted (no update follows it, CEF 156), so
    // TorFox deletes the file itself. Chromium's record of the download is
    // erased when TorFox exits (chromium_profile::ScrubDownloadRecords).
    e->state = State::kFailed;
    LogFinished(*e, static_cast<int>(item->GetInterruptReason()));
    RemoveLeftover(e->id, e->partial, 0);
  } else if (e->state == State::kInProgress || e->state == State::kAsking) {
    const bool disabled = Decide() == downloads::Decision::kBlock &&
                          !fault_injection::SkipDownloadUpdateGuard();
    if (e->cancel_requested || disabled) {
      CancelEntry(*e, e->cancel_requested ? "user" : "downloads disabled");
    }
  }
  Notify();
  return e;
}

void DownloadService::LogFinished(const Entry& e, int reason) {
  test_log::Event(e.state == State::kComplete    ? "download_complete"
                  : e.state == State::kCancelled ? "download_cancelled"
                                                 : "download_failed",
                  {{"id", e.id}, {"name", e.name}, {"bytes", e.received}, {"reason", reason}});
}

void DownloadService::FinishFile(uint32_t id, int attempt) {
  CEF_REQUIRE_UI_THREAD();
  Entry* e = Find(id);
  if (!e || !e->finishing) {
    return;  // Removed from the list meanwhile (the file stays as it is).
  }
  std::error_code ec;
  if (e->partial.empty()) {
    ec = std::make_error_code(std::errc::invalid_argument);
  } else {
    // If something took the final name meanwhile, pick another.
    if (std::filesystem::exists(e->path, ec)) {
      const std::filesystem::path dir = e->path.parent_path();
      std::optional<std::filesystem::path> other = downloads::UniquePath(
          dir, platform::PathToUtf8(e->path.filename()),
          [this](const std::filesystem::path& p) {
            std::error_code x;
            return std::filesystem::exists(p, x) || std::filesystem::exists(PartialPath(p), x) ||
                   PathInUse(p);
          },
          downloads::kPartialSuffix.size());
      if (other) {
        e->path = *other;
        e->name = platform::PathToUtf8(other->filename());
      }
    }
    ec.clear();
    // A rename within the folder keeps the file's "downloaded from the
    // internet" mark (Zone.Identifier / com.apple.quarantine).
    std::filesystem::rename(e->partial, e->path, ec);
  }
  if (ec) {
    if (attempt < 40) {
      std::weak_ptr<int> alive = alive_;
      CefPostDelayedTask(TID_UI,
                         base::BindOnce(
                             [](std::weak_ptr<int> alive, DownloadService* self, uint32_t id,
                                int attempt) {
                               if (alive.lock()) {
                                 self->FinishFile(id, attempt);
                               }
                             },
                             alive, this, id, attempt + 1),
                         250);
      return;
    }
    e->finishing = false;
    e->state = State::kFailed;
    test_log::Event("download_rename_failed", {{"id", id}, {"error", ec.message()}});
    LogFinished(*e, -1);
    Notify();
    return;
  }
  e->finishing = false;
  e->state = State::kComplete;
  LogFinished(*e, 0);
  Notify();
}

void DownloadService::CancelEntry(Entry& e, const std::string& why) {
  if (Finished(e.state)) {
    return;
  }
  e.cancel_requested = true;
  if (e.control) {
    e.control->Cancel();
  }
  e.state = State::kCancelled;
  test_log::Event("download_cancelled", {{"id", e.id}, {"why", why}, {"known", true}});
  RemoveLeftover(e.id, e.partial, 0);
}

void DownloadService::RemoveLeftover(uint32_t id, std::filesystem::path partial, int attempt) {
  CEF_REQUIRE_UI_THREAD();
  if (partial.empty()) {
    return;
  }
  if (attempt > 0) {
    std::error_code ec;
    if (PathInUse(partial) || !std::filesystem::exists(partial, ec)) {
      return;  // Chromium deleted it, or the name belongs to another download now.
    }
    const bool removed = std::filesystem::remove(partial, ec);
    if (removed || attempt >= 10) {
      test_log::Event("download_leftover_removed", {{"id", id}, {"ok", removed}});
      return;
    }
  }
  // Chromium deletes the file on its file thread, after the cancel.
  std::weak_ptr<int> alive = alive_;
  CefPostDelayedTask(TID_UI,
                     base::BindOnce(
                         [](std::weak_ptr<int> alive, DownloadService* self, uint32_t id,
                            std::filesystem::path partial, int attempt) {
                           if (alive.lock()) {
                             self->RemoveLeftover(id, std::move(partial), attempt);
                           }
                         },
                         alive, this, id, std::move(partial), attempt + 1),
                     attempt == 0 ? 2000 : 1000);
}

bool DownloadService::Cancel(uint32_t id) {
  Entry* e = Find(id);
  if (!e || Finished(e->state)) {
    return false;
  }
  CancelEntry(*e, "user");
  Notify();
  return true;
}

bool DownloadService::Remove(uint32_t id) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const Entry& e) { return e.id == id; });
  if (it == entries_.end() || !Finished(it->state)) {
    return false;
  }
  entries_.erase(it);
  Notify();
  return true;
}

void DownloadService::ClearFinished(const Viewer& viewer) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return Finished(e.state) && Visible(e, viewer);
                                }),
                 entries_.end());
  Notify();
}

bool DownloadService::Visible(const Entry& e, const Viewer& viewer) {
  if (viewer.private_window) {
    return e.private_window && e.window_id == viewer.window_id;
  }
  return !e.private_window;
}

DownloadService::Entry* DownloadService::FindVisible(uint32_t id, const Viewer& viewer) {
  Entry* e = Find(id);
  return e && Visible(*e, viewer) ? e : nullptr;
}

void DownloadService::CancelAll(const std::string& why) {
  for (Entry& e : entries_) {
    CancelEntry(e, why);
  }
  Notify();
}

void DownloadService::ForgetWindow(int window_id) {
  for (Entry& e : entries_) {
    if (e.window_id == window_id && e.private_window) {
      CancelEntry(e, "private window closed");
    }
  }
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return e.window_id == window_id && e.private_window;
                                }),
                 entries_.end());
  Notify();
}

int DownloadService::CancelForTab(int window_id, int tab_id, const std::string& why) {
  int n = 0;
  for (Entry& e : entries_) {
    if (e.window_id == window_id && (tab_id == 0 || e.tab_id == tab_id) && !Finished(e.state) &&
        !e.finishing) {
      CancelEntry(e, why);
      ++n;
    }
  }
  if (n > 0) {
    Notify();
  }
  return n;
}

int DownloadService::ActiveCount(const Viewer& viewer) const {
  return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
    return !Finished(e.state) && Visible(e, viewer);
  }));
}

int DownloadService::VisibleCount(const Viewer& viewer) const {
  return static_cast<int>(std::count_if(entries_.begin(), entries_.end(),
                                        [&](const Entry& e) { return Visible(e, viewer); }));
}

const char* DownloadService::StateName(State s) {
  switch (s) {
    case State::kAsking:
      return "asking";
    case State::kInProgress:
      return "in_progress";
    case State::kComplete:
      return "complete";
    case State::kCancelled:
      return "cancelled";
    case State::kFailed:
      return "failed";
  }
  return "failed";
}

Json DownloadService::EntryJson(const Entry& e) {
  return {{"id", e.id},
          {"url", e.url},
          {"origin", e.origin},
          {"name", e.name},
          {"received", e.received},
          {"total", e.total},
          {"size", downloads::FormatSize(e.total >= 0 ? e.total : e.received)},
          {"state", StateName(e.state)},
          {"private", e.private_window},
          {"started", e.started_ms}};
}

Json DownloadService::ListJson(const Viewer& viewer) const {
  Json out = Json::array();
  // Newest first.
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (Visible(*it, viewer)) {
      out.push_back(EntryJson(*it));
    }
  }
  return out;
}

int DownloadService::AddObserver(Observer observer) {
  const int id = next_observer_id_++;
  observers_[id] = std::move(observer);
  return id;
}

void DownloadService::RemoveObserver(int id) {
  observers_.erase(id);
}

void DownloadService::Notify() {
  // Observers may remove themselves.
  std::vector<Observer> copy;
  for (auto& [id, o] : observers_) {
    copy.push_back(o);
  }
  for (Observer& o : copy) {
    o();
  }
}

}  // namespace torfox
