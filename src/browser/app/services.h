// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_SERVICES_H_
#define TORFOX_BROWSER_APP_SERVICES_H_

#include <functional>
#include <memory>
#include <string>

#include "include/wrapper/cef_message_router.h"

namespace torfox {

class ContextManager;
class DownloadService;
class PlacesService;
class PrivacyService;
class SettingsService;
class TorService;
class BrowserWindow;

// Browser-process services shared by windows and handlers. Owned by
// BrowserApp; valid on the UI thread between OnContextInitialized and
// shutdown.
struct Services {
  SettingsService* settings = nullptr;
  ContextManager* contexts = nullptr;
  PlacesService* places = nullptr;  // History and bookmarks.
  DownloadService* downloads = nullptr;
  // Settings > Privacy & Security. Also read on the IO thread (tracker
  // blocking): set before the first window opens, cleared after the last.
  PrivacyService* privacy = nullptr;
  // TorFox's tor. Tor windows' tabs keep their own reference and read
  // connected() on the IO thread.
  std::shared_ptr<TorService> tor;
  // Starts tor with the bridges from Settings > Tor, if it is not running.
  std::function<void()> tor_start;
  // New identity: closes every Tor window, forgets the Tor context and asks
  // tor for new circuits; then opens a fresh Tor window.
  std::function<void(BrowserWindow* requester)> tor_new_identity;
  // Shown once in the first window (for example: clearing data failed).
  std::string startup_notice;
  CefRefPtr<CefMessageRouterBrowserSide> router;
  std::function<void(BrowserWindow*)> window_opened;
  std::function<void(BrowserWindow*)> window_destroyed;
};

Services& GetServices();

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_SERVICES_H_
