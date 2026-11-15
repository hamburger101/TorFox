// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/bridge/bridge_handler.h"

#include <algorithm>
#include <string>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/browser_registry.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/download_service.h"
#include "src/browser/app/places_service.h"
#include "src/browser/app/services.h"
#include "src/browser/app/tor_service.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/app/release_notes.h"
#include "src/browser/app/update_service.h"
#include "src/browser/app/version.h"
#include "src/browser/platform/platform.h"
#include "src/browser/shell/browser_window.h"
#include "src/core/update/whats_new.h"

namespace torfox {

namespace {

constexpr int kRejected = 403;
constexpr int kFailed = 400;

void Reply(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, const Json& value) {
  callback->Success(DumpJson(value));
}

std::string ChromiumVersion() {
  return std::to_string(CHROME_VERSION_MAJOR) + "." + std::to_string(CHROME_VERSION_MINOR) + "." +
         std::to_string(CHROME_VERSION_BUILD) + "." + std::to_string(CHROME_VERSION_PATCH);
}

}  // namespace

BridgeHandler::BridgeHandler() {
  Services& services = GetServices();
  if (services.places) {
    places_observer_ = services.places->AddObserver([this](PlacesService::Change change) {
      NotifyPlaces(change == PlacesService::Change::kBookmarks);
    });
  }
  if (services.settings) {
    settings_observer_ = services.settings->AddObserver([this](const std::string& key) {
      if (key == "appearance.theme") {
        PushAppearance();
      }
    });
  }
  if (services.downloads) {
    downloads_observer_ = services.downloads->AddObserver([this]() { NotifyDownloads(); });
  }
}

BridgeHandler::~BridgeHandler() = default;

void BridgeHandler::Shutdown() {
  Services& services = GetServices();
  if (services.settings && settings_observer_) {
    services.settings->RemoveObserver(settings_observer_);
  }
  settings_observer_ = 0;
  if (services.places && places_observer_) {
    services.places->RemoveObserver(places_observer_);
  }
  places_observer_ = 0;
  if (services.downloads && downloads_observer_) {
    services.downloads->RemoveObserver(downloads_observer_);
  }
  downloads_observer_ = 0;
  if (services.updates && updates_observer_) {
    services.updates->RemoveObserver(updates_observer_);
  }
  updates_observer_ = 0;
  update_subscribers_.clear();
  downloads_subscribers_.clear();
  appearance_subscribers_.clear();
  bookmarks_subscribers_.clear();
  history_subscribers_.clear();
}

void BridgeHandler::NotifyPlaces(bool bookmarks) {
  const std::string payload = DumpJson({{"changed", bookmarks ? "bookmarks" : "history"}});
  for (Subscriber& s : bookmarks ? bookmarks_subscribers_ : history_subscribers_) {
    s.callback->Success(payload);
  }
}

void BridgeHandler::NotifyDownloads() {
  // Each page asks again with downloads.list, which shows only what its
  // window may see; the push itself carries nothing.
  const std::string payload = DumpJson({{"changed", "downloads"}});
  for (Subscriber& s : downloads_subscribers_) {
    s.callback->Success(payload);
  }
}

bool BridgeHandler::OnQuery(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            int64_t query_id,
                            const CefString& request,
                            bool persistent,
                            CefRefPtr<Callback> callback) {
  CEF_REQUIRE_UI_THREAD();
  const int browser_id = browser->GetIdentifier();
  auto entry = Registry().Find(browser_id);
  const std::string frame_url = frame->GetURL().ToString();

  bridge::Context ctx;
  ctx.sender_is_internal = entry && IsInternalRole(entry->role);
  ctx.is_main_frame = frame->IsMain();
  ctx.frame_url = frame_url;
  ctx.persistent = persistent;
  bridge::Validated msg = bridge::Validate(bridge::TorFoxPages(), ctx, request.ToString());

  // The page must also match the browser's role: only the shell browser may
  // speak for torfox://shell/, only the suggestion list for
  // torfox://suggest/, and each of them for nothing else.
  if (msg.ok() && !guards::PageAllowedForRole(msg.page, GuardRole(entry->role))) {
    msg.reject = bridge::Reject::kBadOrigin;
    msg.detail = "page does not match browser role";
  }
  if (!msg.ok()) {
    test_log::Event("bridge_reject", {{"reason", std::string(bridge::RejectName(msg.reject))},
                                      {"detail", msg.detail},
                                      {"browser", browser_id},
                                      {"url", frame_url.substr(0, 200)}});
    callback->Failure(kRejected, "rejected");
    return true;
  }
  test_log::Event("bridge_ok", {{"page", msg.page}, {"cmd", msg.command}});
  Dispatch(msg, browser, query_id, callback);
  return true;
}

bool BridgeHandler::OnQuery(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            int64_t query_id,
                            CefRefPtr<const CefBinaryBuffer> request,
                            bool persistent,
                            CefRefPtr<Callback> callback) {
  // The UI never sends binary messages.
  test_log::Event("bridge_reject",
                  {{"reason", "binary_message"}, {"browser", browser->GetIdentifier()}});
  callback->Failure(kRejected, "rejected");
  return true;
}

void BridgeHandler::OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    int64_t query_id) {
  const int browser_id = browser->GetIdentifier();
  for (std::vector<Subscriber>* list :
       {&appearance_subscribers_, &bookmarks_subscribers_, &history_subscribers_,
        &downloads_subscribers_, &update_subscribers_}) {
    list->erase(std::remove_if(list->begin(), list->end(),
                               [&](const Subscriber& s) {
                                 return s.browser_id == browser_id && s.query_id == query_id;
                               }),
                list->end());
  }
  if (auto entry = Registry().Find(browser_id); entry && entry->window) {
    entry->window->RemoveStateSubscriber(browser_id, query_id);
  }
}

namespace {

Json UpdateStatusJson() {
  UpdateService* updates = GetServices().updates;
  if (!updates) {
    return {{"state", "starting"}, {"installed", kTorFoxVersion}};
  }
  const UpdateService::Status s = updates->status();
  return {{"state", UpdateService::StateName(s.state)},
          {"message", s.message},
          {"version", s.version},
          {"notes", s.notes},
          {"size", s.size},
          {"received", s.received},
          {"lastCheck", s.last_check},
          {"installed", kTorFoxVersion},
          {"platform", platform::UpdatePlatform()}};
}

}  // namespace

void BridgeHandler::NotifyUpdates() {
  const std::string payload = DumpJson(UpdateStatusJson());
  for (Subscriber& s : update_subscribers_) {
    s.callback->Success(payload);
  }
}

bool BridgeHandler::DispatchUpdates(const bridge::Validated& msg, int browser_id,
                                    int64_t query_id, CefRefPtr<Callback> callback) {
  const std::string& cmd = msg.command;
  if (cmd.rfind("update.", 0) != 0) {
    return false;
  }
  UpdateService* updates = GetServices().updates;
  if (cmd == "update.subscribe") {
    if (updates && !updates_observer_) {
      updates_observer_ = updates->AddObserver([this] { NotifyUpdates(); });
    }
    update_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
    Reply(callback, UpdateStatusJson());
    return true;
  }
  if (!updates) {
    callback->Failure(kFailed, "not_ready");
    return true;
  }
  if (cmd == "update.check") {
    updates->Check();
  } else if (cmd == "update.download") {
    updates->Download();
  } else if (cmd == "update.install") {
    // "Relaunch to update", from the toolbar or Settings > Updates. A prompt
    // it shows belongs to the sender: the shell's active tab, or that tab.
    auto entry = Registry().Find(browser_id);
    BrowserWindow* window = entry ? entry->window : nullptr;
    if (!window || !GetServices().relaunch_to_update) {
      callback->Failure(kFailed, "no window");
      return true;
    }
    const int tab_id = entry->role == BrowserRole::kShell ? window->active_tab_id() : entry->tab_id;
    GetServices().relaunch_to_update(window, tab_id,
                                     [callback](bool ok, const std::string& error) {
                                       if (ok) {
                                         Reply(callback, UpdateStatusJson());
                                       } else {
                                         callback->Failure(kFailed, error);
                                       }
                                     });
    return true;
  }
  Reply(callback, UpdateStatusJson());
  return true;
}

void BridgeHandler::PushAppearance() {
  Services& services = GetServices();
  if (!services.settings) {
    return;
  }
  const Json payload = {{"theme", services.settings->store().GetString("appearance.theme")},
                        {"dark", services.settings->EffectiveDark()}};
  for (Subscriber& s : appearance_subscribers_) {
    Reply(s.callback, payload);
  }
}

void BridgeHandler::Dispatch(const bridge::Validated& msg,
                             CefRefPtr<CefBrowser> browser,
                             int64_t query_id,
                             CefRefPtr<Callback> callback) {
  Services& services = GetServices();
  const int browser_id = browser->GetIdentifier();
  auto entry = Registry().Find(browser_id);
  BrowserWindow* window = entry ? entry->window : nullptr;
  const std::string& cmd = msg.command;

  auto window_result = [&](const BrowserWindow::Result& r) {
    if (r.ok) {
      Reply(callback, Json::object());
    } else {
      callback->Failure(kFailed, r.error);
    }
  };

  if (cmd == "appearance.subscribe") {
    appearance_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
    Reply(callback, {{"theme", services.settings->store().GetString("appearance.theme")},
                     {"dark", services.settings->EffectiveDark()}});
    return;
  }
  if (cmd == "settings.get") {
    Reply(callback, services.settings->DescribeForUi());
    return;
  }
  if (cmd == "settings.set") {
    const std::string key = GetString(msg.args, "key").value_or("");
    const Json* value = GetMember(msg.args, "value");
    auto result = services.settings->Change(key, value ? *value : Json());
    if (result == SettingsService::ChangeResult::kOk ||
        result == SettingsService::ChangeResult::kUnchanged) {
      Reply(callback, {{"value", services.settings->store().Get(key)}});
    } else {
      callback->Failure(kFailed, std::string(ChangeResultName(result)));
    }
    return;
  }
  if (cmd == "memory.query") {
    // Listing processes takes a few milliseconds: not on the UI thread.
    std::vector<memory::Renderer> renderers =
        services.collect_renderers ? services.collect_renderers() : std::vector<memory::Renderer>();
    const std::filesystem::path tor_dir =
        services.tor ? services.tor->program_dir() : platform::BundledTorPaths().dir;
    CefPostTask(TID_FILE_USER_VISIBLE,
                base::BindOnce(
                    [](std::vector<memory::Renderer> renderers, std::filesystem::path tor_dir,
                       CefRefPtr<Callback> callback) {
                      Json report =
                          memory::BuildReport(platform::TorFoxProcesses(tor_dir), renderers);
                      CefPostTask(TID_UI, base::BindOnce(
                                              [](CefRefPtr<Callback> callback, Json report) {
                                                Reply(callback, report);
                                              },
                                              callback, std::move(report)));
                    },
                    std::move(renderers), tor_dir, callback));
    return;
  }
  if (cmd == "whatsnew.get") {
    // The page TorFox opens after an update: this version's bundled notes,
    // shown as text.
    Reply(callback, {{"version", kTorFoxVersion},
                     {"previous", services.whats_new_from},
                     {"verified", services.whats_new_verified},
                     {"notes", update::NormalizeNotes(ReleaseNotes())}});
    return;
  }
  if (cmd == "app.info") {
    Reply(callback, {{"name", "TorFox"},
                     {"version", kTorFoxVersion},
                     {"cef", CEF_VERSION},
                     {"chromium", ChromiumVersion()},
                     {"license", "MPL-2.0"},
                     {"torBundle", kTorBundleVersion},
                     {"tor", services.tor ? services.tor->version() : std::string()},
                     {"testBuild", kTestHooks}});
    return;
  }

  if (DispatchPlaces(msg, browser_id, query_id, callback)) {
    return;
  }
  if (DispatchUpdates(msg, browser_id, query_id, callback)) {
    return;
  }

  // Everything below acts on the sender's window.
  if (!window) {
    callback->Failure(kFailed, "no window");
    return;
  }
  if (DispatchDownloads(msg, window, browser_id, query_id, callback)) {
    return;
  }
  // From the shell: the active tab. From an internal tab: that tab.
  const int sender_tab = entry->role == BrowserRole::kShell ? 0 : entry->tab_id;
  if (cmd == "state.subscribe") {
    window->AddStateSubscriber(browser_id, query_id, callback);
  } else if (cmd == "suggest.query") {
    Reply(callback, window->SuggestQuery(GetString(msg.args, "text").value_or(""),
                                         static_cast<int>(GetInt(msg.args, "x").value_or(0)),
                                         static_cast<int>(GetInt(msg.args, "y").value_or(0)),
                                         static_cast<int>(GetInt(msg.args, "width").value_or(1))));
  } else if (cmd == "suggest.highlight") {
    window->SuggestHighlight(static_cast<int>(GetInt(msg.args, "index").value_or(-1)));
    Reply(callback, Json::object());
  } else if (cmd == "suggest.hide") {
    window->SuggestHide();
    Reply(callback, Json::object());
  } else if (cmd == "suggest.subscribe") {
    window->AddSuggestSubscriber(browser_id, query_id, callback);
  } else if (cmd == "suggest.open") {
    window_result(window->SuggestOpen(static_cast<int>(GetInt(msg.args, "index").value_or(-1))));
  } else if (cmd == "tab.new") {
    window->NewTab();
    Reply(callback, Json::object());
  } else if (cmd == "tab.close") {
    window_result(window->CloseTab(static_cast<int>(GetInt(msg.args, "id").value_or(0))));
  } else if (cmd == "tab.select") {
    window_result(window->SelectTab(static_cast<int>(GetInt(msg.args, "id").value_or(0))));
  } else if (cmd == "tab.move") {
    window_result(window->MoveTab(static_cast<int>(GetInt(msg.args, "id").value_or(0)),
                                  static_cast<int>(GetInt(msg.args, "index").value_or(0))));
  } else if (cmd == "tab.reopen") {
    window->ReopenClosedTab();
    Reply(callback, Json::object());
  } else if (cmd == "window.new" || cmd == "window.newPrivate") {
    window->HandleCommand({cmd == "window.new" ? browsing::Command::kNewWindow
                                               : browsing::Command::kNewPrivateWindow});
    Reply(callback, Json::object());
  } else if (cmd == "window.newTor") {
    window->HandleCommand({browsing::Command::kNewTorWindow});
    Reply(callback, Json::object());
  } else if (cmd == "tor.newIdentity") {
    window->HandleCommand({browsing::Command::kNewTorIdentity});
    Reply(callback, Json::object());
  } else if (cmd == "tor.connect") {
    // "Try again" after an error; does nothing while tor is running.
    if (GetServices().tor_start) {
      GetServices().tor_start();
    }
    Reply(callback, Json::object());
  } else if (cmd == "nav.go") {
    window_result(window->Navigate(sender_tab, GetString(msg.args, "input").value_or("")));
  } else if (cmd == "nav.open") {
    const bool new_tab = GetMember(msg.args, "newTab") && msg.args["newTab"].get<bool>();
    window_result(window->OpenUrl(sender_tab, GetString(msg.args, "url").value_or(""), new_tab));
  } else if (cmd == "nav.home") {
    window->Home();
    Reply(callback, Json::object());
  } else if (cmd == "find.query") {
    const Json* forward = GetMember(msg.args, "forward");
    const Json* match_case = GetMember(msg.args, "matchCase");
    window->Find(GetString(msg.args, "text").value_or(""), !forward || forward->get<bool>(),
                 match_case && match_case->get<bool>());
    Reply(callback, Json::object());
  } else if (cmd == "find.stop") {
    window->StopFind();
    Reply(callback, Json::object());
  } else if (cmd == "zoom.change") {
    window->ChangeZoom(static_cast<int>(GetInt(msg.args, "step").value_or(0)));
    Reply(callback, Json::object());
  } else if (cmd == "bookmarks.star") {
    window_result(window->StarActivePage(static_cast<int>(GetInt(msg.args, "x").value_or(0)),
                                         static_cast<int>(GetInt(msg.args, "y").value_or(0))));
  } else if (cmd == "adblock.menu") {
    window_result(window->ShowShieldMenu(static_cast<int>(GetInt(msg.args, "x").value_or(0)),
                                         static_cast<int>(GetInt(msg.args, "y").value_or(0))));
  } else if (cmd == "bookmarks.folderMenu") {
    window_result(window->ShowBookmarkFolderMenu(
        GetInt(msg.args, "id").value_or(0), static_cast<int>(GetInt(msg.args, "x").value_or(0)),
        static_cast<int>(GetInt(msg.args, "y").value_or(0))));
  } else if (cmd == "app.menu") {
    window->ShowAppMenu(static_cast<int>(GetInt(msg.args, "x").value_or(0)),
                        static_cast<int>(GetInt(msg.args, "y").value_or(0)));
    Reply(callback, Json::object());
  } else if (cmd == "app.openBookmarks") {
    window->OpenInternalPage("bookmarks");
    Reply(callback, Json::object());
  } else if (cmd == "app.openHistory") {
    window->OpenInternalPage("history");
    Reply(callback, Json::object());
  } else if (cmd == "app.openDownloads") {
    window->OpenInternalPage("downloads");
    Reply(callback, Json::object());
  } else if (cmd == "app.focusContent") {
    window->FocusActiveContent();
    Reply(callback, Json::object());
  } else if (cmd == "nav.back") {
    window->GoBack();
    Reply(callback, Json::object());
  } else if (cmd == "nav.forward") {
    window->GoForward();
    Reply(callback, Json::object());
  } else if (cmd == "nav.reload") {
    window->Reload(false);
    Reply(callback, Json::object());
  } else if (cmd == "nav.stop") {
    window->Stop();
    Reply(callback, Json::object());
  } else if (cmd == "app.openSettings") {
    window->OpenSettings();
    Reply(callback, Json::object());
  } else {
    // Unreachable: the validator only admits commands listed above.
    callback->Failure(kRejected, "rejected");
  }
}

bool BridgeHandler::DispatchDownloads(const bridge::Validated& msg,
                                      BrowserWindow* window,
                                      int browser_id,
                                      int64_t query_id,
                                      CefRefPtr<Callback> callback) {
  const std::string& cmd = msg.command;
  if (cmd.rfind("downloads.", 0) != 0) {
    return false;
  }
  DownloadService* service = GetServices().downloads;
  // The page sees and acts on its own window's downloads only (private
  // downloads never appear in another window).
  const DownloadService::Viewer viewer{window->is_private(), window->id()};
  if (cmd == "downloads.subscribe") {
    downloads_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
    Reply(callback, {{"changed", ""}});
    return true;
  }
  if (!service) {
    callback->Failure(kFailed, "Downloads are unavailable.");
    return true;
  }
  const uint32_t id = static_cast<uint32_t>(GetInt(msg.args, "id").value_or(0));
  if (cmd == "downloads.list") {
    const downloads::Decision decision = service->Decide();
    Reply(callback, {{"entries", service->ListJson(viewer)},
                     {"policy", decision == downloads::Decision::kBlock  ? "disabled"
                                : decision == downloads::Decision::kAsk ? "ask"
                                                                        : "allow"},
                     {"private", viewer.private_window}});
  } else if (cmd == "downloads.cancel") {
    if (!service->FindVisible(id, viewer) || !service->Cancel(id)) {
      callback->Failure(kFailed, "That download is not running.");
      return true;
    }
    Reply(callback, Json::object());
  } else if (cmd == "downloads.remove") {
    if (!service->FindVisible(id, viewer) || !service->Remove(id)) {
      callback->Failure(kFailed, "Only finished downloads can be removed from the list.");
      return true;
    }
    Reply(callback, Json::object());
  } else if (cmd == "downloads.clear") {
    service->ClearFinished(viewer);
    Reply(callback, Json::object());
  } else if (cmd == "downloads.reveal") {
    DownloadService::Entry* e = service->FindVisible(id, viewer);
    if (!e || e->state != DownloadService::State::kComplete) {
      callback->Failure(kFailed, "That file is not available.");
      return true;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(e->path, ec)) {
      callback->Failure(kFailed, "The file has been moved or deleted.");
      return true;
    }
    test_log::Event("download_revealed", {{"id", e->id}});
    if (!kTestHooks) {
      platform::RevealInFolder(e->path);
    }
    Reply(callback, Json::object());
  } else {
    return false;
  }
  return true;
}

bool BridgeHandler::DispatchPlaces(const bridge::Validated& msg,
                                   int browser_id,
                                   int64_t query_id,
                                   CefRefPtr<Callback> callback) {
  const std::string& cmd = msg.command;
  if (cmd.rfind("bookmarks.", 0) != 0 && cmd.rfind("history.", 0) != 0) {
    return false;
  }
  // The shell's star and folder menus act on its window.
  if (cmd == "bookmarks.star" || cmd == "bookmarks.folderMenu") {
    return false;
  }
  PlacesService* service = GetServices().places;
  browsing::Places* places = service ? service->places() : nullptr;
  if (cmd == "bookmarks.subscribe" || cmd == "history.subscribe") {
    (cmd == "bookmarks.subscribe" ? bookmarks_subscribers_ : history_subscribers_)
        .push_back(Subscriber{browser_id, query_id, callback});
    Reply(callback, {{"changed", ""},
                     {"available", places != nullptr},
                     {"notice", service ? service->notice() : std::string()}});
    return true;
  }
  if (!places) {
    callback->Failure(kFailed, service && !service->notice().empty()
                                   ? service->notice()
                                   : std::string("History and bookmarks are unavailable."));
    return true;
  }
  const int64_t now = PlacesService::NowMs();
  auto id_arg = [&msg](const char* name) { return GetInt(msg.args, name).value_or(0); };
  auto str_arg = [&msg](const char* name) { return GetString(msg.args, name).value_or(""); };
  auto changed = [service](bool bookmarks) {
    service->Notify(bookmarks ? PlacesService::Change::kBookmarks
                              : PlacesService::Change::kHistory);
  };
  auto list = [](const std::vector<browsing::Bookmark>& items) {
    Json out = Json::array();
    for (const browsing::Bookmark& b : items) {
      out.push_back(PlacesService::BookmarkJson(b));
    }
    return out;
  };

  if (cmd == "bookmarks.children") {
    auto folder = places->GetBookmark(id_arg("id"));
    if (!folder || folder->kind != browsing::BookmarkKind::kFolder) {
      callback->Failure(kFailed, "That folder no longer exists.");
      return true;
    }
    Reply(callback, {{"folder", PlacesService::BookmarkJson(*folder)},
                     {"items", list(places->Children(folder->id))}});
  } else if (cmd == "bookmarks.folders") {
    // Every folder, depth first, with its depth for indentation.
    Json out = Json::array();
    std::vector<std::pair<int64_t, int>> stack = {{browsing::kOtherBookmarksId, 0},
                                                  {browsing::kBookmarksBarId, 0}};
    while (!stack.empty() && out.size() < 5000) {
      auto [id, depth] = stack.back();
      stack.pop_back();
      auto folder = places->GetBookmark(id);
      if (!folder) {
        continue;
      }
      Json f = PlacesService::BookmarkJson(*folder);
      f["depth"] = depth;
      out.push_back(std::move(f));
      std::vector<browsing::Bookmark> children = places->Children(id);
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (it->kind == browsing::BookmarkKind::kFolder && depth < 32) {
          stack.push_back({it->id, depth + 1});
        }
      }
    }
    Reply(callback, {{"folders", out}});
  } else if (cmd == "bookmarks.search") {
    Reply(callback, {{"items", list(places->SearchBookmarks(str_arg("query"), 500))}});
  } else if (cmd == "bookmarks.add") {
    const std::string url = str_arg("url");
    if (!browsing::IsStorableUrl(url)) {
      callback->Failure(kFailed, "Only http and https addresses can be bookmarked.");
      return true;
    }
    auto id = places->AddBookmark(id_arg("parent"), str_arg("title"), url,
                                  static_cast<int>(GetInt(msg.args, "index").value_or(-1)), now);
    if (!id) {
      callback->Failure(kFailed, "The bookmark could not be added.");
      return true;
    }
    changed(true);
    Reply(callback, {{"id", *id}});
  } else if (cmd == "bookmarks.addFolder") {
    auto id = places->AddFolder(id_arg("parent"), str_arg("title"),
                                static_cast<int>(GetInt(msg.args, "index").value_or(-1)), now);
    if (!id) {
      callback->Failure(kFailed, "The folder could not be added.");
      return true;
    }
    changed(true);
    Reply(callback, {{"id", *id}});
  } else if (cmd == "bookmarks.update") {
    auto existing = places->GetBookmark(id_arg("id"));
    if (!existing) {
      callback->Failure(kFailed, "That bookmark no longer exists.");
      return true;
    }
    std::string url = existing->url;
    if (existing->kind == browsing::BookmarkKind::kUrl && GetMember(msg.args, "url")) {
      url = str_arg("url");
      if (!browsing::IsStorableUrl(url)) {
        callback->Failure(kFailed, "Only http and https addresses can be bookmarked.");
        return true;
      }
    }
    if (!places->UpdateBookmark(existing->id, str_arg("title"), url)) {
      callback->Failure(kFailed, "The bookmark could not be changed.");
      return true;
    }
    changed(true);
    Reply(callback, Json::object());
  } else if (cmd == "bookmarks.move") {
    if (!places->MoveBookmark(id_arg("id"), id_arg("parent"),
                              static_cast<int>(GetInt(msg.args, "index").value_or(-1)))) {
      callback->Failure(kFailed, "The bookmark could not be moved there.");
      return true;
    }
    changed(true);
    Reply(callback, Json::object());
  } else if (cmd == "bookmarks.remove") {
    if (!places->RemoveBookmark(id_arg("id"))) {
      callback->Failure(kFailed, "The bookmark could not be removed.");
      return true;
    }
    changed(true);
    Reply(callback, Json::object());
  } else if (cmd == "history.list") {
    const std::string query = str_arg("query");
    const std::size_t limit = static_cast<std::size_t>(GetInt(msg.args, "limit").value_or(100));
    std::vector<browsing::HistoryEntry> entries =
        query.empty() ? places->RecentHistory(limit, GetInt(msg.args, "before").value_or(0))
                      : places->SearchHistory(query, limit);
    Json out = Json::array();
    for (const browsing::HistoryEntry& e : entries) {
      out.push_back(PlacesService::HistoryJson(e));
    }
    Reply(callback, {{"entries", out}, {"more", entries.size() == limit && query.empty()}});
  } else if (cmd == "history.removeUrl") {
    if (!places->DeleteHistoryUrl(str_arg("url"))) {
      callback->Failure(kFailed, "The entry could not be removed.");
      return true;
    }
    test_log::Event("history_removed", {{"what", "url"}});
    changed(false);
    Reply(callback, Json::object());
  } else if (cmd == "history.clear") {
    if (!places->ClearHistory()) {
      callback->Failure(kFailed, "History could not be cleared.");
      return true;
    }
    test_log::Event("history_removed", {{"what", "all"}});
    changed(false);
    std::string error;
    ContextManager* contexts = GetServices().contexts;
    if (contexts && !contexts->ClearSiteRecords(&error)) {
      test_log::Event("site_records_clear_failed", {{"error", error}});
      callback->Failure(kFailed,
                        "History was cleared, but the zoom levels saved for each site could not "
                        "be.");
      return true;
    }
    Reply(callback, Json::object());
  } else {
    return false;
  }
  return true;
}

}  // namespace torfox
