// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/tab_client.h"

#include "include/cef_parser.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/test_log.h"
#include "src/browser/shell/browser_window.h"
#include "src/core/url/guards.h"
#include "src/core/url/internal_url.h"
#include "src/core/url/nav_input.h"

namespace torfox {

namespace {

void LogBlock(const char* what, BrowserRole role, const std::string& url, const std::string& why) {
  test_log::Event(what, {{"role", std::string(RoleName(role))},
                         {"url", url.substr(0, 300)},
                         {"why", why}});
}

}  // namespace

TabClient::TabClient(BrowserRole role,
                     int tab_id,
                     CefRefPtr<BrowserWindow> window,
                     CefRefPtr<CefMessageRouterBrowserSide> router,
                     std::string initial_url)
    : role_(role),
      tab_id_(tab_id),
      window_(window),
      router_(IsInternalRole(role) ? router : nullptr),
      initial_url_(std::move(initial_url)) {}

void TabClient::Close() {
  CEF_REQUIRE_UI_THREAD();
  closing_ = true;
  if (browser_) {
    // Force: no beforeunload prompt can keep a closed tab alive.
    browser_->GetHost()->CloseBrowser(true);
  }
}

bool TabClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         CefProcessId source_process,
                                         CefRefPtr<CefProcessMessage> message) {
  // Web browsers never reach the bridge, whatever their renderer sends.
  if (!router_) {
    test_log::Event("bridge_reject",
                    {{"reason", "message_from_web_browser"}, {"browser", browser->GetIdentifier()}});
    return false;
  }
  return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

// ----- Context menu ----------------------------------------------------------

void TabClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefContextMenuParams> params,
                                    CefRefPtr<CefMenuModel> model) {
  if (IsInternal()) {
    // Keep only editing commands on internal pages (no "View source",
    // "Inspect", "Back"...).
    const int edit_flags = params->GetEditStateFlags();
    model->Clear();
    if (params->IsEditable()) {
      if (edit_flags & CM_EDITFLAG_CAN_CUT) {
        model->AddItem(MENU_ID_CUT, "Cut");
      }
      if (edit_flags & CM_EDITFLAG_CAN_COPY) {
        model->AddItem(MENU_ID_COPY, "Copy");
      }
      if (edit_flags & CM_EDITFLAG_CAN_PASTE) {
        model->AddItem(MENU_ID_PASTE, "Paste");
      }
      model->AddItem(MENU_ID_SELECT_ALL, "Select All");
    } else if (edit_flags & CM_EDITFLAG_CAN_COPY) {
      model->AddItem(MENU_ID_COPY, "Copy");
    }
    return;
  }
  // Web tabs: Stage 2 builds TorFox's own menu. Until then remove entries
  // that would download or save content (download policy arrives in Stage 3).
  for (int id : {MENU_ID_PRINT, MENU_ID_VIEW_SOURCE}) {
    model->Remove(id);
  }
}

// ----- Display ---------------------------------------------------------------

void TabClient::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                const CefString& url) {
  if (frame->IsMain() && window_ && role_ != BrowserRole::kShell) {
    window_->OnAddressChange(tab_id_, url.ToString());
  }
}

void TabClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
  if (window_ && role_ != BrowserRole::kShell) {
    window_->OnTitleChange(tab_id_, title.ToString());
  }
}

bool TabClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                 cef_log_severity_t level,
                                 const CefString& message,
                                 const CefString& source,
                                 int line) {
  // Internal pages' console output (CSP or Trusted Types violations, UI
  // errors) is recorded for tests. Web console output is never recorded.
  if (IsInternal()) {
    test_log::Event("console", {{"role", std::string(RoleName(role_))},
                                {"level", static_cast<int>(level)},
                                {"message", message.ToString().substr(0, 1000)},
                                {"source", source.ToString().substr(0, 300)},
                                {"line", line}});
  }
  return false;
}

// ----- Downloads (blocked until Stage 3) -------------------------------------

bool TabClient::CanDownload(CefRefPtr<CefBrowser> browser,
                            const CefString& url,
                            const CefString& request_method) {
  LogBlock("download_blocked", role_, url.ToString(), "downloads not implemented yet");
  return false;
}

bool TabClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefDownloadItem> download_item,
                                 const CefString& suggested_name,
                                 CefRefPtr<CefBeforeDownloadCallback> callback) {
  // Defence in depth: never reached because CanDownload refuses. Returning
  // true without continuing the callback cancels the download.
  LogBlock("download_blocked", role_, download_item->GetURL().ToString(), "OnBeforeDownload");
  return true;
}

// ----- Keyboard --------------------------------------------------------------

bool TabClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                              const CefKeyEvent& event,
                              CefEventHandle os_event,
                              bool* is_keyboard_shortcut) {
  if (event.type != KEYEVENT_RAWKEYDOWN || !window_) {
    return false;
  }
#if defined(OS_MAC)
  const uint32_t kPrimary = EVENTFLAG_COMMAND_DOWN;
#else
  const uint32_t kPrimary = EVENTFLAG_CONTROL_DOWN;
#endif
  const uint32_t mods = event.modifiers & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_COMMAND_DOWN |
                                           EVENTFLAG_ALT_DOWN | EVENTFLAG_SHIFT_DOWN);
  const int key = event.windows_key_code;
  auto fire = [this](Shortcut s) {
    CefRefPtr<BrowserWindow> window = window_;
    CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::HandleShortcut, window, s));
    return true;
  };
  if (mods == kPrimary) {
    switch (key) {
      case 'T':
        return fire(Shortcut::kNewTab);
      case 'W':
        return fire(Shortcut::kCloseTab);
      case 'L':
        return fire(Shortcut::kFocusAddress);
      case 'R':
        return fire(Shortcut::kReload);
      case 0xBC:  // VK_OEM_COMMA
        return fire(Shortcut::kSettings);
#if defined(OS_MAC)
      case 0xDB:  // [
        return fire(Shortcut::kBack);
      case 0xDD:  // ]
        return fire(Shortcut::kForward);
#endif
    }
  }
  if (mods == 0 && key == 0x74) {  // F5
    return fire(Shortcut::kReload);
  }
  if (mods == EVENTFLAG_ALT_DOWN && key == 0x25) {  // Alt+Left
    return fire(Shortcut::kBack);
  }
  if (mods == EVENTFLAG_ALT_DOWN && key == 0x27) {  // Alt+Right
    return fire(Shortcut::kForward);
  }
  if (key == 0x09 && (mods == EVENTFLAG_CONTROL_DOWN)) {  // Ctrl+Tab
    return fire(Shortcut::kNextTab);
  }
  if (key == 0x09 && (mods == (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_SHIFT_DOWN))) {
    return fire(Shortcut::kPreviousTab);
  }
  return false;
}

// ----- Life span -------------------------------------------------------------

bool TabClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
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
                              bool* no_javascript_access) {
  const std::string url = target_url.ToString();
  // Popups never become real popup browsers (no opener relationship). Web
  // pages may open a new tab only with a user gesture; Stage 4 adds the
  // configurable popup blocker.
  if (role_ != BrowserRole::kWebTab || !user_gesture) {
    LogBlock("popup_blocked", role_, url, user_gesture ? "internal browser" : "no user gesture");
    return true;
  }
  const std::string scheme = guards::SchemeOf(url);
  if (!NavigationBlockReason(url).empty() || (scheme != "http" && scheme != "https")) {
    LogBlock("popup_blocked", role_, url, "disallowed URL");
    return true;
  }
  if (window_) {
    CefRefPtr<BrowserWindow> window = window_;
    CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::OpenWebUrlInNewTab, window, url, true));
  }
  return true;
}

void TabClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = browser;
  // Register before the first navigation: the browser was created with no
  // URL, so no request can reach the scheme handler before this point.
  Registry().Add(browser->GetIdentifier(), BrowserEntry{role_, window_.get(), tab_id_});
  test_log::Event("browser_created", {{"browser", browser->GetIdentifier()},
                                      {"role", std::string(RoleName(role_))},
                                      {"tab", tab_id_}});
  if (window_) {
    window_->OnClientCreated(this, browser);
  }
  if (closing_) {
    browser->GetHost()->CloseBrowser(true);
    return;
  }
  if (!initial_url_.empty()) {
    browser->GetMainFrame()->LoadURL(initial_url_);
  }
}

bool TabClient::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  // Detach the view and drop every reference to it; the view hierarchy
  // tear-down then completes the close (docs/STAGE0_FINDINGS.md, spike B).
  if (window_) {
    window_->OnClientDoClose(this);
  }
  return true;
}

void TabClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (router_) {
    router_->OnBeforeClose(browser);
  }
  Registry().Remove(browser->GetIdentifier());
  test_log::Event("browser_closed", {{"browser", browser->GetIdentifier()},
                                     {"role", std::string(RoleName(role_))}});
  CefRefPtr<BrowserWindow> window = window_;
  window_ = nullptr;
  router_ = nullptr;
  browser_ = nullptr;
  if (window) {
    window->OnClientClosed(this);
  }
}

// ----- Load ------------------------------------------------------------------

void TabClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                     bool isLoading,
                                     bool canGoBack,
                                     bool canGoForward) {
  if (window_ && role_ != BrowserRole::kShell) {
    window_->OnLoadingState(tab_id_, isLoading, canGoBack, canGoForward);
  }
}

// ----- Permissions (denied until Stage 4) ------------------------------------

bool TabClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               const CefString& requesting_origin,
                                               uint32_t requested_permissions,
                                               CefRefPtr<CefMediaAccessCallback> callback) {
  test_log::Event("permission_denied", {{"kind", "media"},
                                        {"origin", requesting_origin.ToString().substr(0, 200)},
                                        {"mask", requested_permissions}});
  callback->Cancel();
  return true;
}

bool TabClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                       uint64_t prompt_id,
                                       const CefString& requesting_origin,
                                       uint32_t requested_permissions,
                                       CefRefPtr<CefPermissionPromptCallback> callback) {
  test_log::Event("permission_denied", {{"kind", "prompt"},
                                        {"origin", requesting_origin.ToString().substr(0, 200)},
                                        {"mask", requested_permissions}});
  callback->Continue(CEF_PERMISSION_RESULT_DENY);
  return true;
}

// ----- Requests --------------------------------------------------------------

std::string TabClient::NavigationBlockReason(const std::string& url) const {
  return guards::NavigationBlockReason(GuardRole(role_), url);
}

std::string TabClient::ResourceBlockReason(const std::string& url) const {
  return guards::ResourceBlockReason(GuardRole(role_), url);
}

bool TabClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               CefRefPtr<CefRequest> request,
                               bool user_gesture,
                               bool is_redirect) {
  CEF_REQUIRE_UI_THREAD();
  const std::string url = request->GetURL().ToString();
  const std::string why = NavigationBlockReason(url);
  if (!why.empty() && !fault_injection::SkipNavigationGuard()) {
    LogBlock("nav_blocked", role_, url, why);
    return true;  // Cancel.
  }
  if (router_) {
    router_->OnBeforeBrowse(browser, frame);
  }
  return false;
}

bool TabClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 const CefString& target_url,
                                 cef_window_open_disposition_t target_disposition,
                                 bool user_gesture) {
  const std::string url = target_url.ToString();
  if (role_ == BrowserRole::kWebTab && user_gesture && NavigationBlockReason(url).empty() &&
      (guards::SchemeOf(url) == "http" || guards::SchemeOf(url) == "https") && window_) {
    const bool foreground = target_disposition == CEF_WOD_NEW_FOREGROUND_TAB;
    CefRefPtr<BrowserWindow> window = window_;
    CefPostTask(TID_UI,
                base::BindOnce(&BrowserWindow::OpenWebUrlInNewTab, window, url, foreground));
  } else {
    LogBlock("open_url_blocked", role_, url, "not allowed");
  }
  return true;  // Never let CEF handle it.
}

CefRefPtr<CefResourceRequestHandler> TabClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool is_navigation,
    bool is_download,
    const CefString& request_initiator,
    bool& disable_default_handling) {
  return this;
}

void TabClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                          TerminationStatus status,
                                          int error_code,
                                          const CefString& error_string) {
  if (router_) {
    router_->OnRenderProcessTerminated(browser);
  }
  test_log::Event("renderer_terminated",
                  {{"role", std::string(RoleName(role_))}, {"status", static_cast<int>(status)}});
  if (window_) {
    window_->OnRendererCrashed(this);
  }
}

CefResourceRequestHandler::ReturnValue TabClient::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  const std::string url = request->GetURL().ToString();
  const std::string why = ResourceBlockReason(url);
  if (!why.empty() && !fault_injection::SkipResourceGuard()) {
    LogBlock("resource_blocked", role_, url, why);
    return RV_CANCEL;
  }
  return RV_CONTINUE;
}

void TabClient::OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefRequest> request,
                                    bool& allow_os_execution) {
  // External protocol handlers are never launched until Stage 4 adds a
  // confirmation prompt (and they stay blocked in Tor tabs).
  LogBlock("protocol_blocked", role_, request->GetURL().ToString(), "external protocol");
  allow_os_execution = false;
}

}  // namespace torfox
