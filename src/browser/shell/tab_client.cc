// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/tab_client.h"

#include <filesystem>
#include <optional>
#include <system_error>

#include "include/cef_parser.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/download_service.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/services.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"
#include "src/browser/shell/browser_window.h"
#include "src/browser/shell/dialogs.h"
#include "src/browser/shell/menus.h"
#include "src/core/browsing/context_menu.h"
#include "src/core/browsing/places.h"
#include "src/core/browsing/shortcuts.h"
#include "src/core/downloads/policy.h"
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

// TorFox's own context-menu commands for web pages.
enum WebMenuCommand {
  kOpenLinkInNewTab = MENU_ID_USER_FIRST,
  kOpenLinkInPrivateWindow,
  kCopyLinkAddress,
  kOpenImageInNewTab,
  kCopyImageAddress,
  kSearchSelection,
  kBookmarkPage,
};

bool IsWebUrl(const std::string& url) {
  const std::string scheme = guards::SchemeOf(url);
  return scheme == "http" || scheme == "https";
}

}  // namespace

TabClient::TabClient(BrowserRole role,
                     int tab_id,
                     CefRefPtr<BrowserWindow> window,
                     CefRefPtr<CefMessageRouterBrowserSide> router,
                     std::string initial_url)
    : role_(role),
      tab_id_(tab_id),
      download_context_(window && window->is_private() ? window->id() : 0),
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
      model->AddItem(MENU_ID_SELECT_ALL, "Select all");
    } else if (edit_flags & CM_EDITFLAG_CAN_COPY) {
      model->AddItem(MENU_ID_COPY, "Copy");
    }
    return;
  }
  BuildWebMenu(params, model);
}

void TabClient::BuildWebMenu(CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) {
  // TorFox builds the whole menu itself instead of trimming Chromium's: no
  // "Save as", "Print", "View source" or "Inspect", and nothing that would
  // download or save content (the download policy arrives in Stage 3).
  model->Clear();
  const int type = params->GetTypeFlags();
  const int edit = params->GetEditStateFlags();
  auto separate = [&model]() {
    if (model->GetCount() > 0 && model->GetTypeAt(model->GetCount() - 1) != MENUITEMTYPE_SEPARATOR) {
      model->AddSeparator();
    }
  };

  const std::string link = params->GetLinkUrl().ToString();
  if ((type & CM_TYPEFLAG_LINK) && IsWebUrl(link) && NavigationBlockReason(link).empty()) {
    model->AddItem(kOpenLinkInNewTab, "Open link in new tab");
    model->AddItem(kOpenLinkInPrivateWindow, "Open link in private window");
    model->AddItem(kCopyLinkAddress, "Copy link address");
  }

  const std::string source = params->GetSourceUrl().ToString();
  if ((type & CM_TYPEFLAG_MEDIA) && params->GetMediaType() == CM_MEDIATYPE_IMAGE &&
      IsWebUrl(source) && NavigationBlockReason(source).empty()) {
    separate();
    model->AddItem(kOpenImageInNewTab, "Open image in new tab");
    model->AddItem(kCopyImageAddress, "Copy image address");
  }

  if (type & CM_TYPEFLAG_EDITABLE) {
    separate();
    model->AddItem(MENU_ID_UNDO, "Undo");
    model->SetEnabled(MENU_ID_UNDO, (edit & CM_EDITFLAG_CAN_UNDO) != 0);
    model->AddItem(MENU_ID_REDO, "Redo");
    model->SetEnabled(MENU_ID_REDO, (edit & CM_EDITFLAG_CAN_REDO) != 0);
    model->AddSeparator();
    model->AddItem(MENU_ID_CUT, "Cut");
    model->SetEnabled(MENU_ID_CUT, (edit & CM_EDITFLAG_CAN_CUT) != 0);
    model->AddItem(MENU_ID_COPY, "Copy");
    model->SetEnabled(MENU_ID_COPY, (edit & CM_EDITFLAG_CAN_COPY) != 0);
    model->AddItem(MENU_ID_PASTE, "Paste");
    model->SetEnabled(MENU_ID_PASTE, (edit & CM_EDITFLAG_CAN_PASTE) != 0);
    model->AddItem(MENU_ID_SELECT_ALL, "Select all");
  } else if (type & CM_TYPEFLAG_SELECTION) {
    separate();
    model->AddItem(MENU_ID_COPY, "Copy");
  }

  // Never offered for anything that mentions an onion address.
  const std::string selection = params->GetSelectionText().ToString();
  if ((type & CM_TYPEFLAG_SELECTION) && browsing::SelectionSearchUrl(selection)) {
    model->AddItem(kSearchSelection, "Search DuckDuckGo for \xE2\x80\x9C" +
                                         browsing::MenuExcerpt(selection, 32) + "\xE2\x80\x9D");
  }

  if (model->GetCount() == 0) {
    // The page itself.
    model->AddItem(MENU_ID_BACK, "Back");
    model->SetEnabled(MENU_ID_BACK, browser_ && browser_->CanGoBack());
    model->AddItem(MENU_ID_FORWARD, "Forward");
    model->SetEnabled(MENU_ID_FORWARD, browser_ && browser_->CanGoForward());
    model->AddItem(MENU_ID_RELOAD, "Reload");
    model->AddSeparator();
    model->AddItem(kBookmarkPage, "Bookmark this page");
    model->SetEnabled(kBookmarkPage, browsing::IsStorableUrl(params->GetPageUrl().ToString()));
    model->AddItem(MENU_ID_SELECT_ALL, "Select all");
  }
}

bool TabClient::RunContextMenu(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               CefRefPtr<CefContextMenuParams> params,
                               CefRefPtr<CefMenuModel> model,
                               CefRefPtr<CefRunContextMenuCallback> callback) {
  // Test builds only (--torfox-test-menu-choice): log the items and pick one
  // without showing a menu. Otherwise CEF shows the native menu.
  auto choice = menus::TestChoice(model, IsInternal() ? "internal_context" : "web_context");
  if (!choice) {
    return false;
  }
  if (*choice >= 0) {
    callback->Continue(*choice, EVENTFLAG_NONE);
  } else {
    callback->Cancel();
  }
  return true;
}

bool TabClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefContextMenuParams> params,
                                     int command_id,
                                     EventFlags event_flags) {
  if (IsInternal()) {
    return false;  // Only CEF's own editing commands are on internal menus.
  }
  const std::string link = params->GetLinkUrl().ToString();
  const std::string source = params->GetSourceUrl().ToString();
  // Every URL is checked again here: the menu was built from the same
  // params, but nothing is opened without the navigation policy's consent.
  auto allowed = [this](const std::string& url) {
    return IsWebUrl(url) && NavigationBlockReason(url).empty();
  };
  switch (command_id) {
    case kOpenLinkInNewTab:
      if (allowed(link)) {
        OpenInNewTab(link, false);
      }
      return true;
    case kOpenLinkInPrivateWindow:
      if (allowed(link)) {
        CefPostTask(TID_UI, base::BindOnce(
                                [](std::string url) { BrowserWindow::Open(url, true); }, link));
      }
      return true;
    case kCopyLinkAddress:
      if (IsWebUrl(link)) {
        platform::CopyTextToClipboard(link);
      }
      return true;
    case kOpenImageInNewTab:
      if (allowed(source)) {
        OpenInNewTab(source, false);
      }
      return true;
    case kCopyImageAddress:
      if (IsWebUrl(source)) {
        platform::CopyTextToClipboard(source);
      }
      return true;
    case kSearchSelection:
      if (auto url = browsing::SelectionSearchUrl(params->GetSelectionText().ToString())) {
        OpenInNewTab(*url, true);
      }
      return true;
    case kBookmarkPage:
      if (window_) {
        CefRefPtr<BrowserWindow> window = window_;
        CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::HandleCommand, window,
                                           browsing::ShortcutCommand{
                                               browsing::Command::kBookmarkPage, 0}));
      }
      return true;
  }
  return false;  // CEF's own commands (copy, paste, back...).
}

void TabClient::OpenInNewTab(const std::string& url, bool foreground) {
  if (!window_) {
    return;
  }
  CefRefPtr<BrowserWindow> window = window_;
  CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::OpenWebUrlInNewTab, window, url, foreground,
                                     tab_id_));
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

// ----- Downloads ---------------------------------------------------------------
//
// One decision (downloads::Decide, from Settings > Downloads) is applied
// three times: in CanDownload, before Chromium starts the download; in
// OnBeforeDownload, before any file is created; and in OnDownloadUpdated,
// which cancels running downloads once downloads are disabled. Only web tabs
// ever download.

namespace {

downloads::Decision CurrentDownloadDecision() {
  DownloadService* d = GetServices().downloads;
  return d ? d->Decide() : downloads::Decision::kBlock;
}

}  // namespace

bool TabClient::CanDownload(CefRefPtr<CefBrowser> browser,
                            const CefString& url,
                            const CefString& request_method) {
  const std::string target = url.ToString();
  if (role_ != BrowserRole::kWebTab) {
    LogBlock("download_blocked", role_, target, "not a web tab");
    return false;
  }
  if (fault_injection::SkipDownloadGuard()) {
    return true;  // Test builds only: OnBeforeDownload's check, on its own.
  }
  if (CurrentDownloadDecision() == downloads::Decision::kBlock) {
    LogBlock("download_blocked", role_, target, "downloads disabled");
    if (window_) {
      window_->ShowNotice("Downloads are turned off in Settings.");
    }
    return false;
  }
  return true;
}

bool TabClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefDownloadItem> download_item,
                                 const CefString& suggested_name,
                                 CefRefPtr<CefBeforeDownloadCallback> callback) {
  // Returning true without ever running |callback| cancels the download.
  const std::string url = download_item->GetURL().ToString();
  DownloadService* service = GetServices().downloads;
  const downloads::Decision decision = CurrentDownloadDecision();
  if (role_ != BrowserRole::kWebTab || !window_ || !service ||
      decision == downloads::Decision::kBlock) {
    // The second layer: in CEF 156 every download the tests can start is
    // refused by CanDownload first (docs/STAGE3_FINDINGS.md), so this check
    // is tested with CanDownload switched off (--torfox-test-skip-guards).
    LogBlock("download_blocked", role_, url,
             decision == downloads::Decision::kBlock ? "downloads disabled (before download)"
                                                     : "not a web tab");
    if (window_ && role_ == BrowserRole::kWebTab) {
      window_->ShowNotice("Downloads are turned off in Settings.");
    }
    return true;
  }
  const std::filesystem::path dir = service->Directory();
  std::error_code ec;
  if (!service->staging_ready() || dir.empty() ||
      (!std::filesystem::is_directory(dir, ec) && !std::filesystem::create_directories(dir, ec))) {
    LogBlock("download_blocked", role_, url, "no download folder");
    window_->ShowNotice("TorFox could not find your Downloads folder.");
    return true;
  }
  if (DownloadService::Entry* known = service->FindItem(download_context_, download_item->GetId())) {
    // Chromium restarts a download that failed and cannot be resumed, and
    // asks for its target again. It keeps the name it was given, and the
    // user's answer if TorFox asked.
    if (known->state == DownloadService::State::kInProgress && !known->cancel_requested &&
        !known->finishing) {
      test_log::Event("download_restarted", {{"id", known->id}});
      callback->Continue(platform::PathToUtf8(known->partial), false);
    } else {
      // Still waiting for an answer, or already ended: no second start.
      known->cancel_requested = true;
      LogBlock("download_blocked", role_, url, "restarted while not running");
    }
    return true;
  }
  const std::string name = downloads::SafeFileName(suggested_name.ToString());
  std::optional<std::filesystem::path> path = downloads::UniquePath(
      dir, name,
      [service](const std::filesystem::path& p) {
        std::error_code e;
        // Chromium's partial file sits beside the target; avoid both, and
        // names other downloads are waiting to use.
        return std::filesystem::exists(p, e) ||
               std::filesystem::exists(DownloadService::PartialPath(p), e) ||
               service->PathInUse(p);
      },
      downloads::kPartialSuffix.size());
  if (!path) {
    LogBlock("download_blocked", role_, url, "no free file name");
    window_->ShowNotice("TorFox could not find a free file name in your Downloads folder.");
    return true;
  }

  DownloadService::Entry entry;
  entry.item_id = download_item->GetId();
  entry.context = download_context_;
  entry.url = url;
  entry.origin = downloads::DisplayOrigin(url);
  if (entry.origin.empty() && browser && browser->GetMainFrame()) {
    // data: and other origin-less URLs: show the page that started it.
    entry.origin = downloads::DisplayOrigin(browser->GetMainFrame()->GetURL().ToString());
  }
  entry.name = platform::PathToUtf8(path->filename());
  entry.path = *path;
  entry.partial = DownloadService::PartialPath(*path);
  entry.total = download_item->GetTotalBytes();
  entry.private_window = window_->is_private();
  entry.window_id = window_->id();
  entry.tab_id = tab_id_;
  entry.state = decision == downloads::Decision::kAllow ? DownloadService::State::kInProgress
                                                        : DownloadService::State::kAsking;
  DownloadService::Entry& added = service->Add(entry);
  const uint32_t id = added.id;
  test_log::Event("download_named",
                  {{"id", id}, {"item", entry.item_id}, {"context", entry.context},
                   {"suggested", suggested_name.ToString()}, {"name", entry.name}});
  // Chromium writes to the partial name; DownloadService renames the file
  // once it is complete.
  const std::string target = platform::PathToUtf8(entry.partial);

  if (decision == downloads::Decision::kAllow) {
    test_log::Event("download_started", {{"id", id}, {"name", entry.name}, {"asked", false}});
    callback->Continue(target, false);
    service->Notify();
    return true;
  }

  platform::ConfirmPrompt prompt;
  prompt.title = "Download this file?";
  prompt.heading = entry.name;
  prompt.details = downloads::FormatSize(entry.total) +
                   (entry.origin.empty() ? std::string() : "\nFrom " + entry.origin);
  if (downloads::IsDangerousType(entry.name)) {
    prompt.warning = "This type of file can run programs on your computer. Download it only if "
                     "you trust where it comes from.";
  }
  prompt.accept = "Download";
  prompt.cancel = "Cancel";
  service->Notify();
  window_->ConfirmDownload(tab_id_, prompt, [callback, id, target, name = entry.name](bool ok) {
    DownloadService* d = GetServices().downloads;
    DownloadService::Entry* e = d ? d->Find(id) : nullptr;
    // Checked again: the setting may have changed while the dialog was open.
    if (!ok || !e || e->cancel_requested || d->Decide() == downloads::Decision::kBlock) {
      if (e) {
        e->cancel_requested = true;
        if (e->control) {
          e->control->Cancel();
        }
        e->state = DownloadService::State::kCancelled;
      }
      test_log::Event("download_cancelled", {{"id", id}, {"why", ok ? "changed" : "declined"},
                                             {"known", e != nullptr}});
      if (d) {
        d->Notify();
      }
      return;  // |callback| is dropped without Continue: Chromium cancels.
    }
    e->state = DownloadService::State::kInProgress;
    test_log::Event("download_started", {{"id", id}, {"name", name}, {"asked", true}});
    callback->Continue(target, false);
    d->Notify();
  });
  return true;
}

void TabClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefDownloadItem> download_item,
                                  CefRefPtr<CefDownloadItemCallback> callback) {
  if (DownloadService* d = GetServices().downloads) {
    d->Update(download_context_, download_item, callback);
  } else if (callback) {
    callback->Cancel();
  }
}

// ----- File dialogs ------------------------------------------------------------

bool TabClient::OnFileDialog(CefRefPtr<CefBrowser> browser,
                             FileDialogMode mode,
                             const CefString& title,
                             const CefString& default_file_path,
                             const std::vector<CefString>& accept_filters,
                             const std::vector<CefString>& accept_extensions,
                             const std::vector<CefString>& accept_descriptions,
                             CefRefPtr<CefFileDialogCallback> callback) {
  // A save dialog (for example a page's showSaveFilePicker) writes a file
  // without the download path, so "Disable downloads" refuses it here.
  if (mode == FILE_DIALOG_SAVE &&
      (role_ != BrowserRole::kWebTab ||
       CurrentDownloadDecision() == downloads::Decision::kBlock)) {
    test_log::Event("file_dialog_refused", {{"mode", static_cast<int>(mode)},
                                            {"why", role_ != BrowserRole::kWebTab
                                                        ? "not a web tab"
                                                        : "downloads disabled"}});
    if (window_ && role_ == BrowserRole::kWebTab) {
      window_->ShowNotice("Downloads are turned off in Settings.");
    }
    callback->Cancel();
    return true;
  }
  if (auto scripted = dialogs::TestFileChoice("file", static_cast<int>(mode))) {
    if (scripted->empty()) {
      callback->Cancel();
    } else {
      std::vector<CefString> paths(scripted->begin(), scripted->end());
      callback->Continue(paths);
    }
    return true;
  }
  return false;  // Chromium's native file dialog.
}

// ----- Find ------------------------------------------------------------------

void TabClient::OnFindResult(CefRefPtr<CefBrowser> browser,
                             int identifier,
                             int count,
                             const CefRect& selectionRect,
                             int activeMatchOrdinal,
                             bool finalUpdate) {
  if (window_ && role_ != BrowserRole::kShell) {
    window_->OnFindResult(tab_id_, count, activeMatchOrdinal, finalUpdate);
  }
}

// ----- Keyboard --------------------------------------------------------------

bool TabClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                              const CefKeyEvent& event,
                              CefEventHandle os_event,
                              bool* is_keyboard_shortcut) {
  if (event.type != KEYEVENT_RAWKEYDOWN || !window_) {
    return false;
  }
  uint32_t mods = 0;
  if (event.modifiers & EVENTFLAG_SHIFT_DOWN) {
    mods |= browsing::keymod::kShift;
  }
  if (event.modifiers & EVENTFLAG_CONTROL_DOWN) {
    mods |= browsing::keymod::kCtrl;
  }
  if (event.modifiers & EVENTFLAG_ALT_DOWN) {
    mods |= browsing::keymod::kAlt;
  }
  if (event.modifiers & EVENTFLAG_COMMAND_DOWN) {
    mods |= browsing::keymod::kCmd;
  }
#if defined(OS_MAC)
  constexpr browsing::KeyPlatform kPlatform = browsing::KeyPlatform::kMac;
#else
  constexpr browsing::KeyPlatform kPlatform = browsing::KeyPlatform::kWindows;
#endif
  auto command = browsing::MapShortcut(event.windows_key_code, mods, kPlatform);
  if (!command) {
    return false;
  }
  CefRefPtr<BrowserWindow> window = window_;
  CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::HandleCommand, window, *command));
  return true;
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
  OpenInNewTab(url, true);
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

void TabClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            TransitionType transition_type) {
  if (frame->IsMain()) {
    test_log::Event("load_start", {{"role", std::string(RoleName(role_))}, {"tab", tab_id_}});
  }
}

void TabClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          int httpStatusCode) {
  if (!frame->IsMain() || role_ != BrowserRole::kWebTab || !window_) {
    return;
  }
  const std::string url = frame->GetURL().ToString();
  test_log::Event("load_end", {{"tab", tab_id_}, {"status", httpStatusCode},
                               {"error_page", url == failed_url_}});
  // Error pages are not visits: no HTTP response, or the URL that just failed.
  if (httpStatusCode <= 0 || url == failed_url_) {
    return;
  }
  window_->OnPageLoaded(tab_id_, url);
}

void TabClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            ErrorCode errorCode,
                            const CefString& errorText,
                            const CefString& failedUrl) {
  if (!frame->IsMain()) {
    return;
  }
  failed_url_ = failedUrl.ToString();
  test_log::Event("load_error", {{"role", std::string(RoleName(role_))},
                                 {"tab", tab_id_},
                                 {"code", static_cast<int>(errorCode)}});
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
    OpenInNewTab(url, target_disposition == CEF_WOD_NEW_FOREGROUND_TAB);
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
