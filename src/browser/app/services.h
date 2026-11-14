// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_SERVICES_H_
#define TORFOX_BROWSER_APP_SERVICES_H_

#include <functional>

#include "include/wrapper/cef_message_router.h"

namespace torfox {

class ContextManager;
class PlacesService;
class SettingsService;
class BrowserWindow;

// Browser-process services shared by windows and handlers. Owned by
// BrowserApp; valid on the UI thread between OnContextInitialized and
// shutdown.
struct Services {
  SettingsService* settings = nullptr;
  ContextManager* contexts = nullptr;
  PlacesService* places = nullptr;  // History and bookmarks.
  CefRefPtr<CefMessageRouterBrowserSide> router;
  std::function<void(BrowserWindow*)> window_opened;
  std::function<void(BrowserWindow*)> window_destroyed;
};

Services& GetServices();

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_SERVICES_H_
