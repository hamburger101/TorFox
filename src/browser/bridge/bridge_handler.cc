// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/bridge/bridge_handler.h"

#include <algorithm>
#include <string>

#include "include/cef_version.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/browser_registry.h"
#include "src/browser/app/services.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/app/version.h"
#include "src/browser/shell/browser_window.h"

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
  if (services.settings) {
    settings_observer_ = services.settings->AddObserver([this](const std::string& key) {
      if (key == "appearance.theme") {
        PushAppearance();
      }
    });
  }
}

BridgeHandler::~BridgeHandler() = default;

void BridgeHandler::Shutdown() {
  Services& services = GetServices();
  if (services.settings && settings_observer_) {
    services.settings->RemoveObserver(settings_observer_);
  }
  settings_observer_ = 0;
  appearance_subscribers_.clear();
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
  // speak for torfox://shell/, and the shell browser for nothing else.
  if (msg.ok() && ((msg.page == "shell") != (entry->role == BrowserRole::kShell))) {
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
  appearance_subscribers_.erase(
      std::remove_if(appearance_subscribers_.begin(), appearance_subscribers_.end(),
                     [&](const Subscriber& s) {
                       return s.browser_id == browser_id && s.query_id == query_id;
                     }),
      appearance_subscribers_.end());
  if (auto entry = Registry().Find(browser_id); entry && entry->window) {
    entry->window->RemoveStateSubscriber(browser_id, query_id);
  }
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
  if (cmd == "app.info") {
    Reply(callback, {{"name", "TorFox"},
                     {"version", kTorFoxVersion},
                     {"cef", CEF_VERSION},
                     {"chromium", ChromiumVersion()},
                     {"license", "MPL-2.0"},
                     {"testBuild", kTestHooks}});
    return;
  }

  // Everything below acts on the sender's window.
  if (!window) {
    callback->Failure(kFailed, "no window");
    return;
  }
  if (cmd == "state.subscribe") {
    window->AddStateSubscriber(browser_id, query_id, callback);
  } else if (cmd == "tab.new") {
    window->NewTab();
    Reply(callback, Json::object());
  } else if (cmd == "tab.close") {
    window_result(window->CloseTab(static_cast<int>(GetInt(msg.args, "id").value_or(0))));
  } else if (cmd == "tab.select") {
    window_result(window->SelectTab(static_cast<int>(GetInt(msg.args, "id").value_or(0))));
  } else if (cmd == "nav.go") {
    // From the shell: the active tab. From an internal tab: that tab.
    const int tab_id = entry->role == BrowserRole::kShell ? 0 : entry->tab_id;
    window_result(window->Navigate(tab_id, GetString(msg.args, "input").value_or("")));
  } else if (cmd == "nav.back") {
    window->GoBack();
    Reply(callback, Json::object());
  } else if (cmd == "nav.forward") {
    window->GoForward();
    Reply(callback, Json::object());
  } else if (cmd == "nav.reload") {
    window->Reload();
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

}  // namespace torfox
