// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_BROWSER_WINDOW_H_
#define TORFOX_BROWSER_SHELL_BROWSER_WINDOW_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "include/cef_request_context.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_message_router.h"
#include "src/browser/app/browser_registry.h"
#include "src/browser/platform/platform.h"
#include "src/core/browsing/shortcuts.h"
#include "src/core/browsing/tabs.h"
#include "src/core/json.h"

namespace torfox {

class TabClient;

// One top-level TorFox window: the shell browser (torfox://shell/) on top and
// a panel holding one BrowserView per tab, of which only the active one is
// visible. A private window owns its own off-the-record request context, so
// its web tabs share nothing with Normal windows or other private windows,
// and it never records history. All methods run on the UI thread.
class BrowserWindow : public CefWindowDelegate {
 public:
  // Creates and shows a new window whose first tab loads |first_url| (a web
  // URL) or the new tab page if empty. A private window first creates its
  // context; if that fails, no window opens and |requester| (if any) tells
  // the user why.
  static void Open(const std::string& first_url, bool private_window = false,
                   CefRefPtr<BrowserWindow> requester = nullptr);

  bool is_private() const { return private_; }
  // Unique for the session; never reused.
  int id() const { return id_; }

  // Asks the user before a download (Settings > Downloads > Ask before
  // download), with a native dialog. |done| runs exactly once on the UI
  // thread: true only if the user chose to download; false if they declined,
  // the dialog failed, or |tab_id|'s tab or this window closed first.
  void ConfirmDownload(int tab_id, const platform::ConfirmPrompt& prompt,
                       std::function<void(bool)> done);
  // A short message in the address bar.
  void ShowNotice(const std::string& text);

  // ----- Called by TabClient -----
  void OnClientCreated(TabClient* client, CefRefPtr<CefBrowser> browser);
  void OnClientDoClose(TabClient* client);
  void OnClientClosed(TabClient* client);
  void OnTitleChange(int tab_id, const std::string& title);
  void OnAddressChange(int tab_id, const std::string& url);
  void OnLoadingState(int tab_id, bool loading, bool can_back, bool can_forward);
  // A main-frame document finished loading with an HTTP response.
  void OnPageLoaded(int tab_id, const std::string& url);
  void OnFindResult(int tab_id, int count, int active, bool final_update);
  void OnRendererCrashed(TabClient* client);
  void OpenWebUrlInNewTab(std::string url, bool foreground, int opener_tab_id);
  void HandleCommand(browsing::ShortcutCommand command);

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
  Result MoveTab(int tab_id, int index);
  void ReopenClosedTab();
  // |tab_id| 0 means the active tab.
  Result Navigate(int tab_id, const std::string& input);
  // Opens a stored bookmark or history URL, in |tab_id| (0: active) or a new
  // foreground tab. Classified exactly like typed input.
  Result OpenUrl(int tab_id, const std::string& url, bool new_tab);
  void GoBack();
  void GoForward();
  void Reload(bool bypass_cache = false);
  void Stop();
  void Home();
  void OpenSettings();
  void OpenInternalPage(std::string_view page);
  void Find(const std::string& text, bool forward, bool match_case);
  void StopFind();
  void ChangeZoom(int step);
  // The star in the address bar: bookmarks the page, or, if it is already
  // bookmarked, shows a menu at |x|,|y| (shell coordinates) to remove or edit.
  Result StarActivePage(int x, int y);
  void ShowAppMenu(int x, int y);
  Result ShowBookmarkFolderMenu(int64_t folder_id, int x, int y);
  void FocusActiveContent();

  // CefWindowDelegate
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override;
  cef_runtime_style_t GetWindowRuntimeStyle() override;

  void ApplyTheme();

 private:
  BrowserWindow(std::string first_url, bool private_window,
                CefRefPtr<CefRequestContext> web_context);

  struct Tab {
    int id = 0;
    int opener = 0;  // Tab that opened this one (links), or 0.
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
  // |index| < 0 appends.
  int AddTab(BrowserRole role, const std::string& url, bool select, int index = -1,
             int opener = 0);
  void LoadInTab(Tab& tab, BrowserRole role, const std::string& url);
  void StartClose(CefRefPtr<TabClient> client, CefRefPtr<CefBrowserView> view,
                  CefRefPtr<CefPanel> parent);
  Tab* FindTab(int tab_id);
  Tab* ActiveTab();
  int IndexOf(int tab_id) const;
  void Activate(int tab_id);
  void SelectTabNumber(int number);
  void MoveActiveTab(int delta);
  void UpdateTitle();
  void RelayoutShell();
  bool RecordsHistory(const Tab& tab) const;
  bool BookmarksBarVisible() const;
  CefPoint ShellToScreen(int x, int y);
  // Adds |folder|'s children to |model|; URL items get command ids
  // 1..urls->size() (index + 1), sub-folders become submenus.
  void FillBookmarkMenu(CefRefPtr<CefMenuModel> model, int64_t folder, int depth,
                        std::vector<std::string>* urls, int* next_submenu_id);
  Json StateJson();
  Json DownloadsJson() const;
  Json BookmarksBarJson();
  void PushState();
  void PushBookmarksBar();
  void PushEvent(const Json& event);
  // Cancels open download prompts for |tab_id|, or all of them for 0.
  void CancelPrompts(int tab_id);
  CefRefPtr<CefBrowser> ActiveBrowser();

  const std::string first_url_;
  const bool private_;
  const int id_;
  // Web tabs' context: Normal, or this window's own private context.
  CefRefPtr<CefRequestContext> web_context_;
  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefBrowserView> shell_view_;
  CefRefPtr<TabClient> shell_client_;
  CefRefPtr<CefPanel> content_;
  std::shared_ptr<int> shell_height_;
  std::vector<Tab> tabs_;
  browsing::ClosedTabs closed_tabs_;
  int active_tab_id_ = 0;
  int next_tab_id_ = 1;
  std::map<TabClient*, Closing> closing_;
  int live_clients_ = 0;
  bool closing_window_ = false;
  int settings_observer_ = 0;
  int places_observer_ = 0;
  int downloads_observer_ = 0;
  // Open download prompts and the tab each belongs to.
  std::vector<std::pair<int, std::shared_ptr<platform::PromptHandle>>> prompts_;

  // Find bar.
  bool find_open_ = false;
  int find_tab_id_ = 0;
  std::string find_text_;
  bool find_match_case_ = false;

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
