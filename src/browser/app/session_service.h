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
// open when the last one closed. UI thread only.
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
  // At exit: keeps the file if sessions are kept, removes it otherwise.
  void Shutdown();

 private:
  void Save();
  void Remove(const char* why);

  SettingsService* settings_;
  const std::filesystem::path file_;
  Collect collect_;
  int observer_ = 0;
  bool pending_ = false;
  bool shut_down_ = false;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_SESSION_SERVICE_H_
