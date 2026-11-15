// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_DOWNLOAD_SERVICE_H_
#define TORFOX_BROWSER_APP_DOWNLOAD_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_download_handler.h"
#include "include/cef_download_item.h"
#include "src/core/downloads/policy.h"
#include "src/core/json.h"

namespace torfox {

class SettingsService;

// TorFox's view of downloads: the policy decision, the target folder, and
// the list shown on torfox://downloads/. The list lives in memory only; it is
// never written to disk, in normal or private windows. UI thread only.
class DownloadService {
 public:
  enum class State { kAsking, kInProgress, kComplete, kCancelled, kFailed };

  struct Entry {
    uint32_t id = 0;            // TorFox's own id, unique in this session (Add sets it).
    // Chromium's id and the download's context. Chromium numbers downloads
    // per profile, so a normal and a private window can both have a
    // download 1; only the pair is unique (see TabClient::download_context_).
    uint32_t item_id = 0;
    int context = 0;
    std::string url;            // Shown in the list (cut to 2048 bytes).
    std::string origin;         // downloads::DisplayOrigin, may be empty.
    std::string name;           // The file's name.
    std::filesystem::path path; // Where it is (or will be) saved.
    // Where Chromium writes it until it is complete: |path| + ".crdownload",
    // so an unfinished file never has its final name. Renamed on completion.
    std::filesystem::path partial;
    int64_t received = 0;
    int64_t total = -1;
    State state = State::kAsking;
    bool private_window = false;
    int window_id = 0;          // BrowserWindow::id() of the requesting window.
    int tab_id = 0;             // The tab that started it.
    int64_t started_ms = 0;
    bool cancel_requested = false;
    bool finishing = false;          // Complete; being moved to its final name.
    // The latest control callback from OnDownloadUpdated, to cancel with.
    CefRefPtr<CefDownloadItemCallback> control;
  };

  using Observer = std::function<void()>;

  // Which entries a window may see and act on: a normal window sees every
  // normal window's downloads, a private window only its own. (Private
  // downloads never show in another window.)
  struct Viewer {
    bool private_window = false;
    int window_id = 0;
  };

  // |dir_override| (test builds) replaces the platform Downloads folder.
  // |staging_dir| is TorFox's own folder for the bytes Chromium receives
  // before a download is confirmed (see StagingDirectory()).
  DownloadService(SettingsService* settings, std::filesystem::path dir_override,
                  std::filesystem::path staging_dir);
  ~DownloadService();

  // The current decision from Settings > Downloads. kBlock if settings are
  // unavailable (fail closed).
  downloads::Decision Decide() const;

  // The folder downloads are saved to; empty if none can be determined.
  std::filesystem::path Directory() const;

  // Chromium starts writing a download to a temporary file in its default
  // download folder before TorFox has decided, and keeps receiving it while
  // the user answers the prompt (pausing has no effect before the target is
  // chosen; docs/STAGE3_FINDINGS.md). That folder is this one, inside
  // TorFox's profile, so an unconfirmed or declined download never appears
  // in the Downloads folder. Chromium deletes the file when a download is
  // cancelled; anything left over (a crash) is deleted by PrepareStaging()
  // at the next start.
  const std::filesystem::path& StagingDirectory() const { return staging_dir_; }
  // Creates the staging folder and deletes leftover files. False if the
  // folder cannot be used (downloads are then refused).
  bool PrepareStaging();
  bool staging_ready() const { return staging_ready_; }

  // The partial name for |final_path|.
  static std::filesystem::path PartialPath(const std::filesystem::path& final_path);

  // True if an unfinished download (still asking, running or being renamed)
  // will use |path| or its partial name. A name is chosen before the file
  // exists, so two downloads waiting at the same time would otherwise get
  // the same one. Letters are compared without case, as on NTFS and APFS
  // (ASCII only).
  bool PathInUse(const std::filesystem::path& path) const;

  Entry* Find(uint32_t id);
  Entry* FindItem(int context, uint32_t item_id);
  // Adds |entry| with a new id.
  Entry& Add(Entry entry);

  // Applies Chromium's view of a download. Cancels it if the policy is now
  // kBlock or the user asked to cancel. Returns the entry, or null if
  // TorFox doesn't know the download (then it is cancelled: every download
  // TorFox allows has an entry first).
  Entry* Update(int context,
                CefRefPtr<CefDownloadItem> item,
                CefRefPtr<CefDownloadItemCallback> control);

  // User actions from torfox://downloads/.
  bool Cancel(uint32_t id);
  bool Remove(uint32_t id);   // Finished entries only.
  void ClearFinished(const Viewer& viewer);
  // Cancels everything not finished (downloads were just disabled).
  void CancelAll(const std::string& why);
  // Drops the entries of a closed private window, cancelling running ones.
  void ForgetWindow(int window_id);
  // CEF ends a download with the browser that started it, without telling
  // the download handler. A closing tab (tab_id > 0) or window (tab_id == 0)
  // cancels its unfinished downloads here first, so the list stays true.
  // Returns how many were cancelled.
  int CancelForTab(int window_id, int tab_id, const std::string& why);

  static bool Visible(const Entry& e, const Viewer& viewer);
  Entry* FindVisible(uint32_t id, const Viewer& viewer);

  int ActiveCount(const Viewer& viewer) const;
  int VisibleCount(const Viewer& viewer) const;
  std::size_t size() const { return entries_.size(); }

  Json ListJson(const Viewer& viewer) const;
  static Json EntryJson(const Entry& e);
  static const char* StateName(State s);

  int AddObserver(Observer observer);
  void RemoveObserver(int id);
  void Notify();

 private:
  void CancelEntry(Entry& e, const std::string& why);
  // Moves a completed download from its partial name to its final one,
  // retrying for a while if the file is busy (for example a virus scan).
  void FinishFile(uint32_t id, int attempt);
  void LogFinished(const Entry& e, int reason);
  // Chromium deletes a cancelled or failed download's partial file. If it
  // is still there a little later, TorFox deletes it (and logs that it had
  // to), unless another download has taken that name since.
  void RemoveLeftover(uint32_t id, std::filesystem::path partial, int attempt);
  uint32_t next_id_ = 0;
  // Expires with this object; delayed tasks check it.
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  SettingsService* settings_;
  std::filesystem::path dir_override_;
  std::filesystem::path staging_dir_;
  bool staging_ready_ = false;
  std::vector<Entry> entries_;  // Oldest first.
  std::map<int, Observer> observers_;
  int next_observer_id_ = 1;
  int settings_observer_ = 0;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_DOWNLOAD_SERVICE_H_
