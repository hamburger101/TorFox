// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_TAB_CLIENT_H_
#define TORFOX_BROWSER_SHELL_TAB_CLIENT_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_client.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_find_handler.h"
#include "include/views/cef_browser_view.h"
#include "include/wrapper/cef_message_router.h"
#include "src/browser/app/browser_registry.h"
#include "src/browser/platform/platform.h"
#include "src/core/adblock/sites.h"
#include "src/core/privacy/https_first.h"

namespace torfox {

class BrowserWindow;
class TorService;

// The CefClient for exactly one browser. Its role (shell, internal tab, web
// tab) is fixed at construction and decides every policy below. Policies
// that matter for isolation are enforced here, in the browser process:
//  * internal browsers may only show their own torfox:// pages and may not
//    load anything from the network;
//  * web browsers may never navigate to, frame or fetch torfox:// URLs,
//    other privileged schemes, or .onion hosts (outside Tor windows);
//  * a Tor window's web browsers send nothing while tor is not connected,
//    and never ask for this computer or the local network;
//  * only internal browsers' messages ever reach the bridge router.
class TabClient : public CefClient,
                  public CefContextMenuHandler,
                  public CefDialogHandler,
                  public CefDisplayHandler,
                  public CefDownloadHandler,
                  public CefFindHandler,
                  public CefKeyboardHandler,
                  public CefLifeSpanHandler,
                  public CefLoadHandler,
                  public CefPermissionHandler,
                  public CefRequestHandler,
                  public CefResourceRequestHandler {
 public:
  TabClient(BrowserRole role,
            int tab_id,
            CefRefPtr<BrowserWindow> window,
            CefRefPtr<CefMessageRouterBrowserSide> router,
            std::string initial_url);

  BrowserRole role() const { return role_; }
  int tab_id() const { return tab_id_; }
  CefRefPtr<CefBrowser> browser() const { return browser_; }
  bool closing() const { return closing_; }
  void Close();
  // A tab in a Tor window (its context is the Tor context).
  bool tor() const { return tor_; }
  const std::vector<long>& renderer_pids() const { return renderer_pids_; }
  // The main-frame URL held back because tor was not connected (UI
  // thread); the window loads it once tor connects.
  std::string TakeTorPendingUrl();
  // Ads and trackers blocked for the page the tab shows (any thread).
  int ads_blocked() const { return ads_blocked_.load(); }
  int trackers_blocked() const { return trackers_blocked_.load(); }

  // CefClient
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefContextMenuHandler
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override;
  bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefContextMenuParams> params,
                      CefRefPtr<CefMenuModel> model,
                      CefRefPtr<CefRunContextMenuCallback> callback) override;
  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id,
                            EventFlags event_flags) override;

  // CefDisplayHandler
  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override;
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override;

  // CefDownloadHandler: Settings > Downloads (downloads::Decide), applied
  // before the download starts, before a file is created, and while it runs.
  bool CanDownload(CefRefPtr<CefBrowser> browser,
                   const CefString& url,
                   const CefString& request_method) override;
  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDownloadItem> download_item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override;
  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefDownloadItem> download_item,
                         CefRefPtr<CefDownloadItemCallback> callback) override;
  // Which Chromium download numbering this tab's downloads use: 0 for the
  // normal profile, the window's id for a private window (its own
  // off-the-record profile). Fixed when the tab is created.
  int download_context() const { return download_context_; }

  // CefDialogHandler: save dialogs are refused while downloads are disabled.
  bool OnFileDialog(CefRefPtr<CefBrowser> browser,
                    FileDialogMode mode,
                    const CefString& title,
                    const CefString& default_file_path,
                    const std::vector<CefString>& accept_filters,
                    const std::vector<CefString>& accept_extensions,
                    const std::vector<CefString>& accept_descriptions,
                    CefRefPtr<CefFileDialogCallback> callback) override;

  // CefFindHandler
  void OnFindResult(CefRefPtr<CefBrowser> browser,
                    int identifier,
                    int count,
                    const CefRect& selectionRect,
                    int activeMatchOrdinal,
                    bool finalUpdate) override;

  // CefKeyboardHandler
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override;

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override;
  void OnLoadStart(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   TransitionType transition_type) override;
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;
  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override;

  // CefPermissionHandler: everything is denied until Stage 4 adds prompts.
  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      const CefString& requesting_origin,
                                      uint32_t requested_permissions,
                                      CefRefPtr<CefMediaAccessCallback> callback) override;
  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                              uint64_t prompt_id,
                              const CefString& requesting_origin,
                              uint32_t requested_permissions,
                              CefRefPtr<CefPermissionPromptCallback> callback) override;
  void OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                 uint64_t prompt_id,
                                 cef_permission_request_result_t result) override;

  // CefRequestHandler
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;
  bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        const CefString& target_url,
                        cef_window_open_disposition_t target_disposition,
                        bool user_gesture) override;
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override;
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override;

  // CefResourceRequestHandler (IO thread). Web tabs use a handler per
  // request instead, which also knows the request's initiator and calls
  // BeforeResourceLoad.
  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override;
  // Every request policy (IO thread). |initiator| is the origin of the
  // document that made the request, or empty if not known.
  ReturnValue BeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefRequest> request,
                                 const std::string& initiator);
  void OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefRequest> request,
                           bool& allow_os_execution) override;

 private:
  bool IsInternal() const { return IsInternalRole(role_); }
  guards::Role GuardRoleOf() const;
  // Tor windows' web tabs: true unless tor is connected (any thread).
  bool TorHoldsBack() const;
  // Navigation policy, shared by OnBeforeBrowse and OnBeforeResourceLoad.
  // Returns an empty string if |url| is allowed, else the reason.
  std::string NavigationBlockReason(const std::string& url) const;
  std::string ResourceBlockReason(const std::string& url) const;
  void BuildWebMenu(CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model);
  // Opens |url| (from a link or menu) in a new tab next to this one.
  void OpenInNewTab(const std::string& url, bool foreground);
  // HTTPS-First (UI thread). True if the main-frame navigation to |url|
  // must be cancelled: it is being retried over https, or it is the https
  // site redirecting back to http.
  bool HttpsFirst(CefRefPtr<CefFrame> frame, const std::string& url, const std::string& method,
                  bool is_redirect);
  // The https attempt failed: ask before loading the http URL.
  void OfferHttp();
  https_first::Exceptions* HttpsExceptions() const;
  // A mailto: or tel: link: asks, then opens the app (UI thread).
  void OpenExternalApp(CefRefPtr<CefBrowser> browser, const std::string& url, bool user_gesture);
  // Ad blocking (EasyList), for web tabs: whether it applies to a page
  // whose top-level document is |top_url| (off in Settings, or ads allowed
  // on the site). Any thread.
  bool BlocksAdsOn(const std::string& top_url) const;
  // Element hiding for the document |frame| just started loading (UI thread).
  void HideAds(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
  // One more request blocked: counts it and tells the window soon (any
  // thread). ResetBlocked starts the counts again for a new page.
  void CountBlocked(std::atomic<int>& counter);
  void ResetBlocked();
  void NotifyBlocked();  // UI thread.
  // A native question for this tab; |done| always runs (false if no
  // prompt could be shown).
  std::shared_ptr<platform::PromptHandle> Ask(const char* kind,
                                              const platform::ConfirmPrompt& prompt,
                                              std::function<void(bool)> done);

  // A tab of the window (not its shell or suggestion list): the window
  // shows its title, address, loading state and find results.
  bool IsTab() const { return role_ == BrowserRole::kInternalTab || role_ == BrowserRole::kWebTab; }

  const BrowserRole role_;
  const int tab_id_;
  const int download_context_;
  const bool tor_;
  const std::shared_ptr<TorService> tor_service_;  // Tor windows only.
  const std::shared_ptr<const std::atomic<bool>> tor_retired_;  // Tor windows only.
  // The window's sites with ads allowed (web tabs only), read on the
  // network thread; the window may close first.
  const std::shared_ptr<adblock::SiteList> ads_allowed_;
  std::atomic<int> ads_blocked_{0};
  std::atomic<int> trackers_blocked_{0};
  std::atomic<bool> notify_pending_{false};
  std::string tor_pending_url_;
  CefRefPtr<BrowserWindow> window_;  // Cleared in OnBeforeClose.
  CefRefPtr<CefMessageRouterBrowserSide> router_;  // Internal roles only.
  std::string initial_url_;
  CefRefPtr<CefBrowser> browser_;
  bool closing_ = false;
  // The processes this browser's frames said they run in (Settings >
  // Memory); reset with each new main-frame document. UI thread.
  std::vector<long> renderer_pids_;
  // The main-frame URL whose load failed most recently: its error page is
  // not history.
  std::string failed_url_;
  // HTTPS-First: the host being tried over https, and the http URL the
  // page asked for. Empty when no attempt is under way.
  std::string https_host_;
  std::string https_from_;
  bool external_prompt_open_ = false;
  // Open permission prompts by Chromium's prompt id.
  std::map<uint64_t, std::shared_ptr<platform::PromptHandle>> permission_prompts_;

  IMPLEMENT_REFCOUNTING(TabClient);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_SHELL_TAB_CLIENT_H_
