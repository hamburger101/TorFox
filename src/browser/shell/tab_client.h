// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_TAB_CLIENT_H_
#define TORFOX_BROWSER_SHELL_TAB_CLIENT_H_

#include <string>

#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/wrapper/cef_message_router.h"
#include "src/browser/app/browser_registry.h"

namespace torfox {

class BrowserWindow;

// The CefClient for exactly one browser. Its role (shell, internal tab, web
// tab) is fixed at construction and decides every policy below. Policies
// that matter for isolation are enforced here, in the browser process:
//  * internal browsers may only show their own torfox:// pages and may not
//    load anything from the network;
//  * web browsers may never navigate to, frame or fetch torfox:// URLs,
//    other privileged schemes, or .onion hosts;
//  * only internal browsers' messages ever reach the bridge router.
class TabClient : public CefClient,
                  public CefContextMenuHandler,
                  public CefDisplayHandler,
                  public CefDownloadHandler,
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

  // CefClient
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
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

  // CefDownloadHandler: downloads are blocked until Stage 3 implements the
  // download policy (fail closed).
  bool CanDownload(CefRefPtr<CefBrowser> browser,
                   const CefString& url,
                   const CefString& request_method) override;
  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDownloadItem> download_item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override;

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

  // CefResourceRequestHandler (IO thread)
  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override;
  void OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefRequest> request,
                           bool& allow_os_execution) override;

 private:
  bool IsInternal() const { return IsInternalRole(role_); }
  // Navigation policy, shared by OnBeforeBrowse and OnBeforeResourceLoad.
  // Returns an empty string if |url| is allowed, else the reason.
  std::string NavigationBlockReason(const std::string& url) const;
  std::string ResourceBlockReason(const std::string& url) const;

  const BrowserRole role_;
  const int tab_id_;
  CefRefPtr<BrowserWindow> window_;  // Cleared in OnBeforeClose.
  CefRefPtr<CefMessageRouterBrowserSide> router_;  // Internal roles only.
  std::string initial_url_;
  CefRefPtr<CefBrowser> browser_;
  bool closing_ = false;

  IMPLEMENT_REFCOUNTING(TabClient);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_SHELL_TAB_CLIENT_H_
