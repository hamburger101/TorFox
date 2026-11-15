// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_BROWSER_APP_H_
#define TORFOX_BROWSER_APP_BROWSER_APP_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"
#include "src/core/privacy/clear_data.h"
#include "src/core/privacy/https_first.h"

namespace torfox {

class BridgeHandler;
class BrowserWindow;
class ContextManager;
class PlacesService;
class PrivacyService;
class SessionService;
class SettingsService;
class DownloadService;
class TorService;
class UpdateService;

// Test builds only: a local fake GitHub and the key its releases are signed
// with.
struct UpdateTestConfig {
  std::string origin;  // "http://127.0.0.1:<port>": API, downloads and redirects.
  std::vector<std::string> extra_origins;  // Further allowed origins (redirect targets).
  std::string public_key;
  std::string repository;
  int first_check_delay_ms = 0;
};

struct StartupOptions {
  std::filesystem::path user_data_dir;
  std::string start_url;  // Web URL for the first tab, or empty for the new tab page.
  std::filesystem::path download_dir;  // Test builds only; empty = the platform's.
  // Test builds only: the local test sites' http and https ports.
  std::optional<https_first::TestPorts> https_test_ports;
  // Test builds only: run this (a fake tor) instead of the bundled tor.
  std::filesystem::path tor_test_exe;
  std::vector<std::string> tor_test_args;
  std::optional<UpdateTestConfig> update_test;
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

  // What Settings > Privacy & Security > Clear when TorFox closes asked for
  // when Shutdown() ran: cleared once Chromium has let go of the profile.
  clear_data::Categories exit_clear() const { return exit_clear_; }
  // The checked update to start once TorFox has shut down (Settings >
  // Updates > Restart to update), taken when Shutdown() ran.
  struct UpdateHandoff {
    std::filesystem::path file;
    std::string sha256;
    uint64_t size = 0;
  };
  std::optional<UpdateHandoff> update_handoff() const { return update_handoff_; }
  // Shown once in the first window.
  void set_startup_notice(std::string text) { startup_notice_ = std::move(text); }

 private:
  void Fatal(const std::string& message);
  bool ApplyGlobalPrefs(std::string* error);
  void ApplyCookiePolicy(bool block);
  void ApplyProxyMode(bool use_system);
  // Tor (Settings > Tor).
  void StartTor();
  void RestartTorIfRunning();
  void NewTorIdentity(BrowserWindow* requester);
  // Settings > Updates.
  void StartUpdates();
  void CloseAllWindows();

  StartupOptions options_;
  std::unique_ptr<SettingsService> settings_;
  std::unique_ptr<PlacesService> places_;
  std::unique_ptr<DownloadService> downloads_;
  std::unique_ptr<PrivacyService> privacy_;
  std::unique_ptr<ContextManager> contexts_;
  std::shared_ptr<TorService> tor_;
  int tor_settings_observer_ = 0;
  std::unique_ptr<BridgeHandler> bridge_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;
  std::set<BrowserWindow*> windows_;
  std::unique_ptr<SessionService> session_;
  std::unique_ptr<UpdateService> updates_;
  std::optional<UpdateHandoff> update_handoff_;
  bool fatal_ = false;
  clear_data::Categories exit_clear_;
  std::string startup_notice_;
  int proxy_observer_ = 0;

  IMPLEMENT_REFCOUNTING(BrowserApp);
};

// Process entry shared by all platforms and process types.
int TorFoxMain(const CefMainArgs& args, void* sandbox_info);

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_BROWSER_APP_H_
