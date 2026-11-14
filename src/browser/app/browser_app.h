// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_BROWSER_APP_H_
#define TORFOX_BROWSER_APP_BROWSER_APP_H_

#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

namespace torfox {

class BridgeHandler;
class BrowserWindow;
class ContextManager;
class PlacesService;
class SettingsService;
class DownloadService;

struct StartupOptions {
  std::filesystem::path user_data_dir;
  std::string start_url;  // Web URL for the first tab, or empty for the new tab page.
  std::filesystem::path download_dir;  // Test builds only; empty = the platform's.
};

// CefApp for the browser process: command-line policy, startup checks,
// services and windows.
class BrowserApp : public CefApp, public CefBrowserProcessHandler {
 public:
  explicit BrowserApp(StartupOptions options);
  ~BrowserApp() override;

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                   const CefString& current_directory) override;

  // Releases every CEF reference. Must run before CefShutdown().
  void Shutdown();

 private:
  void Fatal(const std::string& message);
  bool ApplyGlobalPrefs(std::string* error);

  StartupOptions options_;
  std::unique_ptr<SettingsService> settings_;
  std::unique_ptr<PlacesService> places_;
  std::unique_ptr<DownloadService> downloads_;
  std::unique_ptr<ContextManager> contexts_;
  std::unique_ptr<BridgeHandler> bridge_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;
  std::set<BrowserWindow*> windows_;
  bool fatal_ = false;

  IMPLEMENT_REFCOUNTING(BrowserApp);
};

// Process entry shared by all platforms and process types.
int TorFoxMain(const CefMainArgs& args, void* sandbox_info);

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_BROWSER_APP_H_
