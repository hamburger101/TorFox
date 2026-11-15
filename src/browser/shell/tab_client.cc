// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/tab_client.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <system_error>

#include "include/cef_parser.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/app_common.h"
#include "src/browser/app/content_filter.h"
#include "src/browser/app/download_service.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/privacy_service.h"
#include "src/browser/app/services.h"
#include "src/browser/app/test_log.h"
#include "src/browser/app/tor_service.h"
#include "src/browser/platform/platform.h"
#include "src/browser/shell/browser_window.h"
#include "src/browser/shell/dialogs.h"
#include "src/browser/shell/menus.h"
#include "src/core/adblock/filters.h"
#include "src/core/browsing/context_menu.h"
#include "src/core/browsing/places.h"
#include "src/core/browsing/shortcuts.h"
#include "src/core/downloads/policy.h"
#include "src/core/privacy/external_protocols.h"
#include "src/core/tor/onion.h"
#include "src/core/url/guards.h"
#include "src/core/url/internal_url.h"
#include "src/core/json.h"
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
  kOpenLinkInTorWindow,
  kCopyLinkAddress,
  kOpenImageInNewTab,
  kCopyImageAddress,
  kSearchSelection,
  kBookmarkPage,
};

// The host of an origin or URL, for prompts ("news.example.com wants ...").
std::string SiteName(const std::string& origin_or_url) {
  const std::string host = guards::HostOf(origin_or_url);
  return host.empty() ? std::string("This page") : host;
}

bool IsWebUrl(const std::string& url) {
  const std::string scheme = guards::SchemeOf(url);
  return scheme == "http" || scheme == "https";
}

// The top-level document of a request's page, as well as the network thread
// knows it: the main frame's address, unless the request itself names
// another site (the tab has just started showing a page of that site; the
// main frame's own requests then name the page as their initiator).
std::string TopUrlOf(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     const std::string& initiator, const std::string& first_party) {
  CefRefPtr<CefFrame> main = browser ? browser->GetMainFrame() : nullptr;
  const std::string main_url = main ? main->GetURL().ToString() : std::string();
  std::string top = content_filter::TopUrl(main_url, first_party);
  if (top != main_url && frame && frame->IsMain()) {
    top = content_filter::TopUrl(initiator, first_party);
  }
  return top;
}

// The document that made a request: its initiator (an origin), or if that
// is unknown or opaque, the nearest frame with a web address.
std::string DocumentUrlOf(const std::string& initiator, CefRefPtr<CefFrame> frame,
                          const std::string& top) {
  if (IsWebUrl(initiator)) {
    return initiator;
  }
  int depth = 0;
  for (CefRefPtr<CefFrame> f = frame; f && depth < 64; f = f->GetParent(), ++depth) {
    std::string url = f->GetURL().ToString();
    if (IsWebUrl(url)) {
      return url;
    }
  }
  return top;
}

// The script that applies a document's element-hiding rules: a constructed
// style sheet, which is not a <style> element in the page. The CSS is a JSON
// string, so it can only ever be data.
std::string HidingScript(const std::string& css) {
  return "(()=>{try{const s=new CSSStyleSheet();s.replaceSync(" + DumpJson(Json(css)) +
         ");document.adoptedStyleSheets=[...document.adoptedStyleSheets,s];}catch(e){}})();";
}

}  // namespace

// Web tabs' handler for one request: OnBeforeResourceLoad is not told the
// request's initiator, which the ad blocker needs.
class WebRequestHandler : public CefResourceRequestHandler {
 public:
  WebRequestHandler(CefRefPtr<TabClient> client, std::string initiator)
      : client_(std::move(client)), initiator_(std::move(initiator)) {}

  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override {
    return client_->BeforeResourceLoad(browser, frame, request, initiator_);
  }
  void OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefRequest> request,
                           bool& allow_os_execution) override {
    client_->OnProtocolExecution(browser, frame, request, allow_os_execution);
  }

 private:
  const CefRefPtr<TabClient> client_;
  const std::string initiator_;
  IMPLEMENT_REFCOUNTING(WebRequestHandler);
};

TabClient::TabClient(BrowserRole role,
                     int tab_id,
                     CefRefPtr<BrowserWindow> window,
                     CefRefPtr<CefMessageRouterBrowserSide> router,
                     std::string initial_url)
    : role_(role),
      tab_id_(tab_id),
      download_context_(window && window->is_private() ? window->id() : 0),
      tor_(window && window->is_tor()),
      tor_service_(tor_ ? GetServices().tor : nullptr),
      tor_retired_(tor_ ? window->tor_retired() : nullptr),
      ads_allowed_(role == BrowserRole::kWebTab && window ? window->ads_allowed() : nullptr),
      window_(window),
      router_(IsInternalRole(role) ? router : nullptr),
      initial_url_(std::move(initial_url)) {}

std::string TabClient::TakeTorPendingUrl() {
  std::string url = std::move(tor_pending_url_);
  tor_pending_url_.clear();
  return url;
}

guards::Role TabClient::GuardRoleOf() const {
  return tor_ && role_ == BrowserRole::kWebTab ? guards::Role::kTorTab : GuardRole(role_);
}

bool TabClient::TorHoldsBack() const {
  if (!tor_ || role_ != BrowserRole::kWebTab) {
    return false;
  }
  // New Identity: this window's context sends nothing more, connected or
  // not (ContextManager::DropTor).
  if (!tor_retired_ || tor_retired_->load()) {
    return true;
  }
  if (fault_injection::SkipTorGuard()) {
    return false;
  }
  return !(tor_service_ && tor_service_->connected());
}

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
  // Any renderer may send this; it is only a hint (app_common.h).
  if (message->GetName() == kRendererPidMessage) {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    const int pid = args && args->GetSize() == 1 && args->GetType(0) == VTYPE_INT ? args->GetInt(0) : 0;
    constexpr std::size_t kMaxPids = 32;
    if (pid > 0) {
      if (frame && frame->IsMain()) {
        renderer_pids_.clear();
      }
      if (std::find(renderer_pids_.begin(), renderer_pids_.end(), pid) == renderer_pids_.end() &&
          renderer_pids_.size() < kMaxPids) {
        renderer_pids_.push_back(pid);
      }
    }
    return true;
  }
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
    // From a Tor window, never into a window that does not use Tor.
    if (!tor_) {
      model->AddItem(kOpenLinkInPrivateWindow, "Open link in private window");
    }
    model->AddItem(kOpenLinkInTorWindow, tor_ ? "Open link in new Tor window" : "Open link in Tor window");
    model->AddItem(kCopyLinkAddress, "Copy link address");
  } else if ((type & CM_TYPEFLAG_LINK) && IsWebUrl(link) && !tor_ &&
             guards::IsOnionHost(guards::HostOf(link))) {
    // An onion link in a normal window: it can only be opened in Tor.
    model->AddItem(kOpenLinkInTorWindow, "Open link in Tor window");
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
    case kOpenLinkInTorWindow:
      // Allowed in a Tor window: the same policy as Tor tabs (onion hosts
      // yes, local addresses no).
      if (IsWebUrl(link) && guards::NavigationBlockReason(guards::Role::kTorTab, link).empty()) {
        CefRefPtr<BrowserWindow> window = window_;
        CefPostTask(TID_UI, base::BindOnce(
                                [](std::string url, CefRefPtr<BrowserWindow> w) {
                                  BrowserWindow::OpenTor(url, w);
                                },
                                link, window));
      }
      return true;
    case kOpenLinkInPrivateWindow:
      if (tor_) {
        return true;  // Never offered in Tor windows.
      }
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
        OpenInNewTab(SearchUrlFor(*url, tor_), true);
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
  if (frame->IsMain() && window_ && IsTab()) {
    window_->OnAddressChange(tab_id_, url.ToString());
  }
}

void TabClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
  if (window_ && IsTab()) {
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
  if (tor_) {
    prompt.warning += std::string(prompt.warning.empty() ? "" : "\n") +
                      "Downloaded over Tor. Opening it can reveal your real address: documents "
                      "and media may load things from the internet without Tor.";
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
  if (window_ && IsTab()) {
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
  // Popups never become real popup browsers (no opener relationship): they
  // open as new tabs. Without a click they are blocked, unless Settings >
  // Privacy & Security > Block pop-ups is off.
  if (role_ != BrowserRole::kWebTab) {
    LogBlock("popup_blocked", role_, url, "internal browser");
    return true;
  }
  PrivacyService* privacy = GetServices().privacy;
  if (!user_gesture && (!privacy || privacy->block_popups())) {
    LogBlock("popup_blocked", role_, url, "no user gesture");
    if (window_) {
      window_->ShowNotice("Pop-up blocked: " + SiteName(url) +
                          " tried to open a window without a click.");
    }
    return true;
  }
  const std::string scheme = guards::SchemeOf(url);
  if (!NavigationBlockReason(url).empty() || (scheme != "http" && scheme != "https")) {
    LogBlock("popup_blocked", role_, url, "disallowed URL");
    return true;
  }
  // Ad pop-ups ($popup rules) are blocked even after a click: that is how
  // most of them open.
  CefRefPtr<CefFrame> main = browser->GetMainFrame();
  const std::string top = main ? main->GetURL().ToString() : std::string();
  if (BlocksAdsOn(top)) {
    const std::string opener = frame ? frame->GetURL().ToString() : std::string();
    adblock::Request ad;
    ad.url = url;
    ad.frame_url = opener;
    ad.top_url = top;
    ad.type = adblock::kPopup;
    if (const adblock::Verdict v = adblock::BuiltIn().Check(ad); v.block) {
      LogBlock("ad_popup_blocked", role_, url, std::string(v.rule));
      CountBlocked(ads_blocked_);
      if (window_) {
        window_->ShowNotice("Ad pop-up blocked: " + SiteName(url));
      }
      return true;
    }
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
  if (window_ && IsTab()) {
    window_->OnLoadingState(tab_id_, isLoading, canGoBack, canGoForward);
  }
}

void TabClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            TransitionType transition_type) {
  if (frame->IsMain()) {
    test_log::Event("load_start", {{"role", std::string(RoleName(role_))}, {"tab", tab_id_}});
  }
  if (role_ == BrowserRole::kWebTab) {
    HideAds(browser, frame);
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
  if (httpStatusCode > 0 && url != failed_url_) {
    https_host_.clear();  // A page loaded: any https attempt is over.
    https_from_.clear();
  }
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
  // The https attempt for an http URL failed. ERR_ABORTED is TorFox's own
  // cancel (or the user's); errors that http cannot fix end the attempt.
  if (!https_host_.empty() && guards::SchemeOf(failed_url_) == "https" &&
      guards::HostOf(failed_url_) == https_host_ && errorCode != ERR_ABORTED) {
    if (https_first::OffersHttpAfter(static_cast<int>(errorCode))) {
      OfferHttp();
    } else {
      https_host_.clear();
      https_from_.clear();
    }
  }
}

// ----- Permissions -----------------------------------------------------------
// Settings > Privacy & Security > Site permissions. Camera, microphone and
// clipboard are blocked, or asked about every time (no answer is stored).
// Everything else (location, notifications, screen capture, ...) is always
// denied.

namespace {

// Chromium records the answer to a permission prompt for the site ("block"
// or "allow", in the profile): later requests would then be answered without
// asking, and the profile would list the sites that asked. TorFox asks every
// time and keeps no per-site answers, so the record is removed straight
// after the answer has been delivered.
void ForgetPermissionAnswer(CefRefPtr<CefBrowser> browser,
                            const std::string& origin,
                            uint32_t permission_types) {
  struct Map {
    uint32_t permission;
    cef_content_setting_types_t setting;
  };
  constexpr Map kMap[] = {
      {CEF_PERMISSION_TYPE_CLIPBOARD, CEF_CONTENT_SETTING_TYPE_CLIPBOARD_READ_WRITE},
      {CEF_PERMISSION_TYPE_GEOLOCATION, CEF_CONTENT_SETTING_TYPE_GEOLOCATION},
      {CEF_PERMISSION_TYPE_NOTIFICATIONS, CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS},
      {CEF_PERMISSION_TYPE_CAMERA_STREAM, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA},
      {CEF_PERMISSION_TYPE_MIC_STREAM, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC},
  };
  if (!browser || origin.empty()) {
    return;
  }
  CefRefPtr<CefRequestContext> context = browser->GetHost()->GetRequestContext();
  CefRefPtr<CefFrame> main = browser->GetMainFrame();
  const std::string top = main ? main->GetURL().ToString() : origin;
  std::vector<cef_content_setting_types_t> settings;
  for (const Map& m : kMap) {
    if (permission_types & m.permission) {
      settings.push_back(m.setting);
    }
  }
  if (!context || settings.empty()) {
    return;
  }
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefRequestContext> context, std::string origin, std::string top,
                             std::vector<cef_content_setting_types_t> settings) {
                            for (cef_content_setting_types_t s : settings) {
                              context->SetContentSetting(origin, top, s,
                                                         CEF_CONTENT_SETTING_VALUE_DEFAULT);
                            }
                            test_log::Event("permission_forgotten",
                                            {{"origin", origin}, {"count", settings.size()}});
                          },
                          context, origin, top, std::move(settings)));
}

}  // namespace

std::shared_ptr<platform::PromptHandle> TabClient::Ask(const char* kind,
                                                       const platform::ConfirmPrompt& prompt,
                                                       std::function<void(bool)> done) {
  if (!window_) {
    CefPostTask(TID_UI, base::BindOnce([](std::function<void(bool)> f) { f(false); },
                                       std::move(done)));
    return nullptr;
  }
  return window_->ConfirmForTab(tab_id_, kind, prompt, std::move(done));
}

bool TabClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               const CefString& requesting_origin,
                                               uint32_t requested_permissions,
                                               CefRefPtr<CefMediaAccessCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  const std::string origin = requesting_origin.ToString().substr(0, 200);
  auto deny = [&](const char* why) {
    test_log::Event("permission_denied", {{"kind", "media"},
                                          {"origin", origin},
                                          {"mask", requested_permissions},
                                          {"why", why}});
    callback->Cancel();
    return true;
  };
  constexpr uint32_t kCamera = CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
  constexpr uint32_t kMicrophone = CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE;
  PrivacyService* privacy = GetServices().privacy;
  if (role_ != BrowserRole::kWebTab || !privacy) {
    return deny("not a web tab");
  }
  if (tor_) {
    return deny("tor window");
  }
  if (requested_permissions & ~(kCamera | kMicrophone)) {
    return deny("screen capture");
  }
  const bool camera = requested_permissions & kCamera;
  const bool microphone = requested_permissions & kMicrophone;
  if ((!camera && !microphone) ||
      (camera && privacy->PermissionFor("permissions.camera") != PrivacyService::Permission::kAsk) ||
      (microphone &&
       privacy->PermissionFor("permissions.microphone") != PrivacyService::Permission::kAsk)) {
    return deny("blocked in settings");
  }
  const std::string what = camera && microphone ? "camera and microphone"
                           : camera             ? "camera"
                                                : "microphone";
  platform::ConfirmPrompt prompt;
  prompt.title = "Allow access to your " + what + "?";
  prompt.heading = SiteName(origin) + " wants to use your " + what;
  prompt.details = "Only this request is allowed: TorFox asks again the next time. The site can use "
                   "your " + what + " until the page stops it or you leave the page.";
  prompt.accept = "Allow";
  prompt.cancel = "Block";
  Ask("permission", prompt, [callback, requested_permissions, origin](bool ok) {
    test_log::Event(ok ? "permission_granted" : "permission_denied",
                    {{"kind", "media"}, {"origin", origin}, {"mask", requested_permissions},
                     {"why", ok ? "allowed by the user" : "declined"}});
    if (ok) {
      callback->Continue(requested_permissions);
    } else {
      callback->Cancel();
    }
  });
  return true;
}

bool TabClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                       uint64_t prompt_id,
                                       const CefString& requesting_origin,
                                       uint32_t requested_permissions,
                                       CefRefPtr<CefPermissionPromptCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  const std::string origin = requesting_origin.ToString().substr(0, 200);
  auto deny = [&](const char* why) {
    test_log::Event("permission_denied", {{"kind", "prompt"},
                                          {"origin", origin},
                                          {"mask", requested_permissions},
                                          {"why", why}});
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    ForgetPermissionAnswer(browser, requesting_origin.ToString(), requested_permissions);
    return true;
  };
  PrivacyService* privacy = GetServices().privacy;
  if (role_ != BrowserRole::kWebTab || !privacy) {
    return deny("not a web tab");
  }
  if (tor_) {
    return deny("tor window");
  }
  if (requested_permissions != CEF_PERMISSION_TYPE_CLIPBOARD) {
    return deny("not available");
  }
  if (privacy->PermissionFor("permissions.clipboard") != PrivacyService::Permission::kAsk) {
    return deny("blocked in settings");
  }
  platform::ConfirmPrompt prompt;
  prompt.title = "Allow access to your clipboard?";
  prompt.heading = SiteName(origin) + " wants to see what you copied";
  prompt.details = "The site could read anything you copied, such as passwords or messages. Only "
                   "this request is allowed: TorFox asks again the next time.";
  prompt.accept = "Allow";
  prompt.cancel = "Block";
  CefRefPtr<TabClient> self = this;
  const std::string full_origin = requesting_origin.ToString();
  std::shared_ptr<platform::PromptHandle> handle =
      Ask("permission", prompt,
          [self, prompt_id, callback, requested_permissions, origin, full_origin](bool ok) {
            self->permission_prompts_.erase(prompt_id);
            test_log::Event(ok ? "permission_granted" : "permission_denied",
                            {{"kind", "prompt"}, {"origin", origin}, {"mask", requested_permissions},
                             {"why", ok ? "allowed by the user" : "declined"}});
            callback->Continue(ok ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
            ForgetPermissionAnswer(self->browser_, full_origin, requested_permissions);
          });
  if (handle) {
    permission_prompts_[prompt_id] = std::move(handle);
  }
  return true;
}

void TabClient::OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                          uint64_t prompt_id,
                                          cef_permission_request_result_t result) {
  // Chromium gave up on the request (the page navigated or closed): so does
  // the question.
  auto it = permission_prompts_.find(prompt_id);
  if (it != permission_prompts_.end()) {
    std::shared_ptr<platform::PromptHandle> handle = std::move(it->second);
    permission_prompts_.erase(it);
    handle->Cancel();
  }
}

// ----- Requests --------------------------------------------------------------

std::string TabClient::NavigationBlockReason(const std::string& url) const {
  return guards::NavigationBlockReason(GuardRoleOf(), url);
}

std::string TabClient::ResourceBlockReason(const std::string& url) const {
  return guards::ResourceBlockReason(GuardRoleOf(), url);
}

bool TabClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               CefRefPtr<CefRequest> request,
                               bool user_gesture,
                               bool is_redirect) {
  CEF_REQUIRE_UI_THREAD();
  const std::string url = request->GetURL().ToString();
  // Links that open another app are not navigations: nothing loads, and
  // mailto:/tel: may open the mail or phone app once the user agrees.
  // Every other scheme is refused by the guard below.
  if (role_ == BrowserRole::kWebTab && !external_protocols::LaunchableScheme(url).empty()) {
    if (tor_) {
      // Another app would open the link without Tor.
      LogBlock("protocol_blocked", role_, url, "tor window");
      if (window_ && user_gesture) {
        window_->ShowNotice("Links that open other apps do not work in Tor windows.");
      }
      return true;
    }
    OpenExternalApp(browser, url, user_gesture);
    return true;
  }
  const std::string why = NavigationBlockReason(url);
  if (!why.empty() && !fault_injection::SkipNavigationGuard()) {
    LogBlock("nav_blocked", role_, url, why);
    if (window_ && frame->IsMain() && user_gesture && role_ == BrowserRole::kWebTab) {
      if (!tor_ && guards::IsOnionHost(guards::HostOf(url))) {
        window_->ShowNotice("Onion sites open only in Tor windows: right-click the link and "
                            "choose Open link in Tor window.");
      } else if (tor_ && guards::IsLocalNetworkHost(guards::HostOf(url))) {
        window_->ShowNotice("Tor windows cannot open addresses on this computer or your local "
                            "network.");
      }
    }
    return true;  // Cancel.
  }
  // Fail closed: nothing leaves a Tor window until tor is connected. The
  // page is loaded once it is.
  if (TorHoldsBack()) {
    LogBlock("tor_blocked", role_, url, "tor not connected");
    if (frame->IsMain()) {
      tor_pending_url_ = url;
      if (window_) {
        window_->OnTorHeldBack(tab_id_);
      }
    }
    return true;
  }
  if (role_ == BrowserRole::kWebTab && frame->IsMain() &&
      HttpsFirst(frame, url, request->GetMethod().ToString(), is_redirect)) {
    return true;  // Cancel: retried over https, or asking first.
  }
  if (router_) {
    router_->OnBeforeBrowse(browser, frame);
  }
  return false;
}

https_first::Exceptions* TabClient::HttpsExceptions() const {
  if (window_ && window_->is_private()) {
    return &window_->https_exceptions();
  }
  PrivacyService* privacy = GetServices().privacy;
  return privacy ? &privacy->normal_exceptions() : nullptr;
}

bool TabClient::HttpsFirst(CefRefPtr<CefFrame> frame,
                           const std::string& url,
                           const std::string& method,
                           bool is_redirect) {
  PrivacyService* privacy = GetServices().privacy;
  https_first::Exceptions* exceptions = HttpsExceptions();
  if (!privacy || !privacy->https_first() || !exceptions || guards::SchemeOf(url) != "http") {
    return false;
  }
  const std::string host = guards::HostOf(url);
  if (exceptions->Contains(host)) {
    return false;  // The user chose http for this site earlier in the session.
  }
  if (!https_host_.empty() && host == https_host_) {
    // The https site sent the browser back to http: it has no working https.
    OfferHttp();
    return true;
  }
  // Only requests that can simply be repeated: a form sent with POST would
  // lose its data (docs/STAGE4_FINDINGS.md).
  if (method != "GET" && method != "HEAD") {
    return false;
  }
  const std::string upgraded = https_first::UpgradedUrl(url, privacy->test_ports());
  if (upgraded.empty()) {
    return false;  // Local names, IP addresses, .onion, unusual ports.
  }
  https_host_ = host;
  https_from_ = url;
  test_log::Event("https_upgraded", {{"tab", tab_id_}, {"from", url.substr(0, 300)},
                                     {"to", upgraded.substr(0, 300)}, {"redirect", is_redirect}});
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefFrame> frame, std::string target) {
                            if (frame->IsValid()) {
                              frame->LoadURL(target);
                            }
                          },
                          frame, upgraded));
  return true;
}

void TabClient::OfferHttp() {
  const std::string host = https_host_;
  const std::string http_url = https_from_;
  https_host_.clear();
  https_from_.clear();
  if (host.empty() || http_url.empty()) {
    return;
  }
  platform::ConfirmPrompt prompt;
  prompt.title = "This site has no secure connection";
  prompt.heading = host + " could not be loaded over HTTPS";
  prompt.details = "Over plain HTTP the page, and anything you type into it, is not encrypted: "
                   "others on the network can read or change it. TorFox remembers your choice "
                   "for this site until it closes.";
  prompt.accept = "Continue to the http site";
  prompt.cancel = "Cancel";
  CefRefPtr<TabClient> self = this;
  Ask("https", prompt, [self, host, http_url](bool ok) {
    test_log::Event("https_fallback", {{"tab", self->tab_id_}, {"accepted", ok}});
    if (!ok || !self->browser_) {
      return;
    }
    if (https_first::Exceptions* exceptions = self->HttpsExceptions()) {
      exceptions->Add(host);
    }
    self->browser_->GetMainFrame()->LoadURL(http_url);
  });
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
  if (role_ == BrowserRole::kWebTab) {
    return new WebRequestHandler(this, request_initiator.ToString());
  }
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
  return BeforeResourceLoad(browser, frame, request, std::string());
}

CefResourceRequestHandler::ReturnValue TabClient::BeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    const std::string& initiator) {
  const std::string url = request->GetURL().ToString();
  const std::string why = ResourceBlockReason(url);
  if (!why.empty() && !fault_injection::SkipResourceGuard()) {
    LogBlock("resource_blocked", role_, url, why);
    return RV_CANCEL;
  }
  if (TorHoldsBack()) {
    LogBlock("tor_blocked", role_, url, "tor not connected");
    return RV_CANCEL;
  }
  if (role_ != BrowserRole::kWebTab) {
    return RV_CONTINUE;
  }
  const cef_resource_type_t type = request->GetResourceType();
  if (type == RT_MAIN_FRAME) {
    // A new page (or a redirect on the way to it): it starts with nothing
    // blocked. TorFox never blocks the page itself.
    ResetBlocked();
    return RV_CONTINUE;
  }
  // Tracker and ad blocking, in every web window (Tor windows included).
  content_filter::Request r;
  r.url = url;
  r.type = type;
  r.first_party = request->GetFirstPartyForCookies().ToString();
  r.top = TopUrlOf(browser, frame, initiator, r.first_party);
  r.document = DocumentUrlOf(initiator, frame, r.top);
  const content_filter::Decision d = content_filter::Check(r, ads_allowed_.get());
  if (d.block) {
    LogBlock(d.ad ? "ad_blocked" : "tracker_blocked", role_, url, d.why);
    CountBlocked(d.ad ? ads_blocked_ : trackers_blocked_);
    return RV_CANCEL;
  }
  return RV_CONTINUE;
}

bool TabClient::BlocksAdsOn(const std::string& top_url) const {
  return role_ == BrowserRole::kWebTab && ads_allowed_ &&
         content_filter::BlocksAdsOn(top_url, ads_allowed_.get());
}

void TabClient::CountBlocked(std::atomic<int>& counter) {
  counter.fetch_add(1);
  // At most one update every 300 ms: a page may block hundreds of requests.
  if (!notify_pending_.exchange(true)) {
    CefPostDelayedTask(TID_UI, base::BindOnce(&TabClient::NotifyBlocked, CefRefPtr<TabClient>(this)),
                       300);
  }
}

void TabClient::ResetBlocked() {
  const int before = ads_blocked_.exchange(0) + trackers_blocked_.exchange(0);
  if (before > 0 && !notify_pending_.exchange(true)) {
    CefPostTask(TID_UI, base::BindOnce(&TabClient::NotifyBlocked, CefRefPtr<TabClient>(this)));
  }
}

void TabClient::NotifyBlocked() {
  CEF_REQUIRE_UI_THREAD();
  notify_pending_ = false;
  if (window_) {
    window_->OnBlockedCountChanged(tab_id_);
  }
}

void TabClient::HideAds(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
  CEF_REQUIRE_UI_THREAD();
  const std::string url = frame->GetURL().ToString();
  if (!IsWebUrl(url)) {
    return;
  }
  CefRefPtr<CefFrame> main = frame->IsMain() ? frame : browser->GetMainFrame();
  const std::string top = frame->IsMain() || !main ? url : main->GetURL().ToString();
  if (!BlocksAdsOn(top)) {
    return;
  }
  const adblock::Engine& engine = adblock::BuiltIn();
  const adblock::PageOptions page = engine.ForPage(url, frame->IsMain() ? std::string() : top);
  const bool top_disabled = !frame->IsMain() && engine.ForPage(top).disabled;
  if (page.disabled || page.no_hiding || top_disabled) {
    return;
  }
  // The rules for every site only in the top-level document: frames get
  // their own site's rules (ad frames are blocked by request rules).
  const bool generic = frame->IsMain() && !page.no_generic_hiding;
  const std::string css = engine.HidingCss(guards::HostOf(url), generic);
  if (css.empty()) {
    return;
  }
  frame->ExecuteJavaScript(HidingScript(css), url, 0);
  test_log::Event("ads_hidden", {{"tab", tab_id_},
                                 {"main", frame->IsMain()},
                                 {"bytes", static_cast<int64_t>(css.size())}});
}

void TabClient::OpenExternalApp(CefRefPtr<CefBrowser> browser,
                                const std::string& url,
                                bool user_gesture) {
  CEF_REQUIRE_UI_THREAD();
  const std::string scheme(external_protocols::LaunchableScheme(url));
  if (scheme.empty() || !window_) {
    LogBlock("protocol_blocked", role_, url, "external protocol");
    return;
  }
  // Only a click can ask: a page cannot pop up questions by itself.
  if (!user_gesture) {
    LogBlock("protocol_blocked", role_, url, "no user gesture");
    return;
  }
  if (external_prompt_open_) {
    LogBlock("protocol_blocked", role_, url, "already asking");
    return;
  }
  const bool mail = scheme == "mailto";
  CefRefPtr<CefFrame> main = browser ? browser->GetMainFrame() : nullptr;
  platform::ConfirmPrompt prompt;
  prompt.title = mail ? "Open your email app?" : "Open your phone app?";
  prompt.heading = SiteName(main ? main->GetURL().ToString() : std::string()) +
                   (mail ? " wants to open your email app" : " wants to open your phone app");
  prompt.details = "Link: " + url.substr(0, 300) +
                   "\nThe app receives this link, and the site cannot see what happens next.";
  prompt.accept = mail ? "Open email app" : "Open phone app";
  prompt.cancel = "Cancel";
  external_prompt_open_ = true;
  CefRefPtr<TabClient> self = this;
  Ask("external", prompt, [self, url, scheme](bool ok) {
    self->external_prompt_open_ = false;
    test_log::Event(ok ? "external_opened" : "external_declined", {{"scheme", scheme}});
    // Test builds log instead: the tests must not start the real mail app.
    if (ok && !kTestHooks && !platform::OpenExternal(url)) {
      if (self->window_) {
        self->window_->ShowNotice("No app could be opened for this link.");
      }
    }
  });
}

void TabClient::OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefRequest> request,
                                    bool& allow_os_execution) {
  // Chromium never launches another app itself. (mailto: and tel: links are
  // handled in OnBeforeBrowse and never get here.)
  allow_os_execution = false;
  LogBlock("protocol_blocked", role_, request->GetURL().ToString(), "external protocol");
}

}  // namespace torfox
