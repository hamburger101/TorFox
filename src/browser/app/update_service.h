// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_UPDATE_SERVICE_H_
#define TORFOX_BROWSER_APP_UPDATE_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/cef_request_context.h"
#include "src/core/update/release.h"

namespace torfox {

class ContextManager;
class SettingsService;

// Settings > Updates: finds, downloads and checks new releases on GitHub
// Releases, and hands a checked one over when TorFox closes.
//
// With Settings > Updates > "Download updates automatically" on (the
// default), a verified update is downloaded as soon as it is found, the
// toolbar offers "Relaunch to update", and on Windows an update that is
// still waiting installs when TorFox closes (without starting it again).
//
// Nothing is trusted until the release manifest's minisign signature has
// verified with the public key compiled into TorFox (config/release.json);
// then the release must be newer than this TorFox and not older than any
// release seen before, and the downloaded file must have the manifest's
// size and SHA-256. Every URL, including each redirect, must be on GitHub's
// hosts. Requests use their own off-the-record context: no cookies, no
// cache, nothing shared with browsing.
//
// UI thread only.
class UpdateService {
 public:
  enum class State {
    kNotConfigured,  // This build has no repository or no public key.
    kOff,            // Settings > Updates is off.
    kIdle,           // Not checked yet in this session.
    kChecking,
    kUpToDate,
    kAvailable,      // A verified, newer release.
    kDownloading,
    kReady,          // Downloaded and checked; installs on a relaunch, or
                     // when TorFox closes (automatic installs, Windows).
    kError,
  };

  struct Config {
    std::string repository;  // "owner/name"
    std::string public_key;  // The "RW..." line of minisign.pub.
    update::Endpoints endpoints;
    std::string platform;  // "windows-x64", ...
    std::string installed_version;
    // The first automatic check, after TorFox starts.
    int first_check_delay_ms = 2 * 60 * 1000;
  };

  struct Status {
    State state = State::kIdle;
    std::string message;  // Errors and reasons, in plain words.
    std::string version;  // kAvailable and later: the new version.
    std::string notes;    // Its release notes (from the signed manifest).
    uint64_t size = 0;
    uint64_t received = 0;
    int64_t last_check = 0;  // Unix seconds; 0 if never.
  };

  // After TorFox has shut down: the checked file to start.
  struct Handoff {
    std::filesystem::path file;
    std::string sha256;
    uint64_t size = 0;
    // True: start TorFox again once installed ("Relaunch to update").
    // False: TorFox was closed with an update waiting; install it only.
    bool relaunch = true;
    std::string version;  // The release's, as the signed manifest gives it.
  };

  UpdateService(SettingsService* settings, ContextManager* contexts,
                const std::filesystem::path& user_data_dir, Config config);
  ~UpdateService();
  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  // Once the browsing contexts exist: removes old downloads and schedules
  // the automatic checks.
  void Start();
  void Check();     // The user asked.
  void Download();  // kAvailable -> kDownloading -> kReady (or kError).
  // kReady: checks the file once more. True if TorFox should now close and
  // hand it over (TakeHandoff()).
  bool PrepareInstall(bool relaunch, std::string* error);
  // TorFox is closing: if an update is ready, automatic installs are on and
  // nothing is handed over yet, prepares it to install without a relaunch.
  bool PrepareInstallOnExit();
  std::optional<Handoff> TakeHandoff();
  void Shutdown();

  Status status() const;
  static const char* StateName(State state);
  int AddObserver(std::function<void()> observer);
  void RemoveObserver(int id);
  // An automatic check found an update and Settings > Updates says to tell
  // the user.
  std::function<void(const std::string& version)> notify;

 private:
  class Fetch;
  using Body = std::function<void(bool ok, int status, const std::string& error,
                                  const std::string& body)>;

  bool Enabled() const;
  bool AutoInstall() const;
  void ApplySettings();
  void Tick(int generation);
  void StartCheck(bool automatic);
  void WithContext(std::function<void()> then);
  void Get(const std::string& url, uint64_t max_bytes, Body done);
  void OnRelease(bool automatic, const update::LatestRelease& latest);
  void OnManifest(bool automatic, const update::LatestRelease& latest,
                  const std::string& signature, const std::string& manifest);
  void Fail(const std::string& message, const char* stage, bool automatic);
  void SetState(State state, std::string message = {});
  // False if update.json could not be written.
  bool SaveState();
  void Notify();
  void CancelFetch();
  std::filesystem::path PartPath() const;
  std::filesystem::path FinalPath() const;

  SettingsService* settings_;
  ContextManager* contexts_;
  const std::filesystem::path dir_;         // <user data>/Updates
  const std::filesystem::path state_file_;  // <user data>/update.json
  Config config_;
  std::optional<update::PublicKey> key_;
  update::Version installed_;
  bool configured_ = false;

  Status status_;
  update::SavedState saved_;
  int64_t last_attempt_ = 0;  // Failed checks too: at most one a day.
  update::Asset asset_;      // kAvailable and later.
  std::string asset_url_;
  std::optional<Handoff> handoff_;

  CefRefPtr<CefRequestContext> context_;
  bool context_pending_ = false;
  std::vector<std::function<void()>> context_waiting_;
  CefRefPtr<Fetch> fetch_;

  int settings_observer_ = 0;
  int generation_ = 0;  // Invalidates scheduled ticks.
  bool shut_down_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::map<int, std::function<void()>> observers_;
  int next_observer_ = 1;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_UPDATE_SERVICE_H_
