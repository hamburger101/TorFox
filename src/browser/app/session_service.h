// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_SESSION_SERVICE_H_
#define TORFOX_BROWSER_APP_SESSION_SERVICE_H_

#include <filesystem>
#include <functional>
#include <memory>

#include "src/core/browsing/session.h"

namespace torfox {

class SettingsService;

// Settings > General > On startup. While "Restore previous tabs" is chosen
// and Settings > Privacy > Clear when TorFox closes > History is off, the
// tabs of TorFox's normal windows are kept in <user data>/session.json,
// written a second after they change, so the next start (also after a
// crash) reopens them. Otherwise that file does not exist: it is removed as
// soon as either setting changes, and again at start and exit. A save that
// would leave no window is skipped, so the file keeps the windows that were
// open when the last one closed.
//
// "Relaunch to update" brings the normal windows' tabs back whatever On
// startup says, as long as history is not cleared when TorFox closes: the
// file is then written for that one start, with a marker
// (<user data>/session-relaunch), and removed once it has been read.
// UI thread only.
class SessionService {
 public:
  // Returns the normal windows' tabs now.
  using Collect = std::function<browsing::Session()>;

  SessionService(SettingsService* settings, std::filesystem::path file, Collect collect);
  ~SessionService();
  SessionService(const SessionService&) = delete;
  SessionService& operator=(const SessionService&) = delete;

  bool enabled() const;
  // The session to reopen at start: empty if there is none or sessions are
  // not kept (the file is then removed).
  browsing::Session TakeStartupSession();
  // A normal window or its tabs changed.
  void Changed();
  // TorFox is about to close for an update and start again: saves the tabs
  // now, for the next start only if sessions are not kept.
  void PrepareRelaunch();
  // At exit: keeps the file if sessions are kept (or a relaunch needs it),
  // removes it otherwise.
  void Shutdown();

 private:
  void Save();
  void Remove(const char* why);

  SettingsService* settings_;
  const std::filesystem::path file_;
  Collect collect_;
  int observer_ = 0;
  std::filesystem::path RelaunchMarker() const;

  bool pending_ = false;
  bool shut_down_ = false;
  bool relaunch_ = false;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_SESSION_SERVICE_H_
