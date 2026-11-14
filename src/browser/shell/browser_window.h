// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_BROWSER_WINDOW_H_
#define TORFOX_BROWSER_SHELL_BROWSER_WINDOW_H_

#include <map>
#include <string>
#include <vector>

#include "include/views/cef_browser_view.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_message_router.h"
#include "src/browser/app/browser_registry.h"
#include "src/core/json.h"

namespace torfox {

class TabClient;

enum class Shortcut {
  kNewTab,
  kCloseTab,
  kFocusAddress,
  kReload,
  kBack,
  kForward,
  kNextTab,
  kPreviousTab,
  kSettings,
};

// One top-level TorFox window: the shell browser (torfox://shell/) on top and
// a panel holding one BrowserView per tab, of which only the active one is
// visible. All methods run on the UI thread.
class BrowserWindow : public CefWindowDelegate {
 public:
  // Creates and shows a new window whose first tab loads |first_url| (a web
  // URL) or the new tab page if empty.
  static void Open(const std::string& first_url);

  // ----- Called by TabClient -----
  void OnClientCreated(TabClient* client, CefRefPtr<CefBrowser> browser);
  void OnClientDoClose(TabClient* client);
  void OnClientClosed(TabClient* client);
  void OnTitleChange(int tab_id, const std::string& title);
  void OnAddressChange(int tab_id, const std::string& url);
  void OnLoadingState(int tab_id, bool loading, bool can_back, bool can_forward);
  void OnRendererCrashed(TabClient* client);
  void OpenWebUrlInNewTab(const std::string& url, bool foreground);
  void HandleShortcut(Shortcut shortcut);

  // ----- Called by the bridge (already validated) -----
  struct Result {
    bool ok = true;
    std::string error;
  };
  void AddStateSubscriber(int browser_id,
                          int64_t query_id,
                          CefRefPtr<CefMessageRouterBrowserSide::Callback> callback);
  void RemoveStateSubscriber(int browser_id, int64_t query_id);
  void NewTab();
  Result CloseTab(int tab_id);
  Result SelectTab(int tab_id);
  // |tab_id| 0 means the active tab.
  Result Navigate(int tab_id, const std::string& input);
  void GoBack();
  void GoForward();
  void Reload();
  void Stop();
  void OpenSettings();

  // CefWindowDelegate
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override;
  cef_runtime_style_t GetWindowRuntimeStyle() override;

  void ApplyTheme();

 private:
  explicit BrowserWindow(std::string first_url);

  struct Tab {
    int id = 0;
    CefRefPtr<TabClient> client;
    CefRefPtr<CefBrowserView> view;
    std::string title;
    std::string url;
    bool loading = false;
    bool can_back = false;
    bool can_forward = false;
  };
  struct Closing {
    CefRefPtr<CefBrowserView> view;
    CefRefPtr<CefPanel> parent;
  };

  CefRefPtr<CefBrowserView> CreateView(BrowserRole role,
                                       int tab_id,
                                       const std::string& url,
                                       CefRefPtr<TabClient>* client_out);
  int AddTab(BrowserRole role, const std::string& url, bool select);
  void LoadInTab(Tab& tab, BrowserRole role, const std::string& url);
  void StartClose(CefRefPtr<TabClient> client, CefRefPtr<CefBrowserView> view,
                  CefRefPtr<CefPanel> parent);
  Tab* FindTab(int tab_id);
  Tab* ActiveTab();
  void Activate(int tab_id);
  void FocusActiveContent();
  void UpdateTitle();
  Json StateJson() const;
  void PushState();
  void PushEvent(const Json& event);
  CefRefPtr<CefBrowser> ActiveBrowser();

  std::string first_url_;
  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefBrowserView> shell_view_;
  CefRefPtr<TabClient> shell_client_;
  CefRefPtr<CefPanel> content_;
  std::vector<Tab> tabs_;
  int active_tab_id_ = 0;
  int next_tab_id_ = 1;
  std::map<TabClient*, Closing> closing_;
  int live_clients_ = 0;
  bool closing_window_ = false;
  int settings_observer_ = 0;

  struct Subscriber {
    int browser_id;
    int64_t query_id;
    CefRefPtr<CefMessageRouterBrowserSide::Callback> callback;
  };
  std::vector<Subscriber> state_subscribers_;

  IMPLEMENT_REFCOUNTING(BrowserWindow);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_SHELL_BROWSER_WINDOW_H_
