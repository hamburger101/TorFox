// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/context_manager.h"

#include <system_error>

#include "include/cef_request_context_handler.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/content_filter.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/privacy_service.h"
#include "src/browser/app/services.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"
#include "src/core/url/guards.h"
#include "src/core/url/internal_url.h"

namespace torfox {

namespace {

CefRefPtr<CefValue> IntValue(int v) {
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetInt(v);
  return value;
}

CefRefPtr<CefValue> StringValue(const std::string& v) {
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetString(v);
  return value;
}

CefRefPtr<CefValue> ProxyValue(const char* mode) {
  CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
  dict->SetString("mode", mode);
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetDictionary(dict);
  return value;
}

// The Tor context's only route: tor's SOCKS port. Chromium resolves names
// at the proxy for socks5:// (the hostname goes to tor, never to local DNS),
// and with a single proxy and no "DIRECT" entry it never falls back to a
// direct connection. "<-loopback>" removes the implicit loopback bypass, so
// even 127.0.0.1 and localhost go to tor, which refuses them.
CefRefPtr<CefValue> TorProxyValue(int port) {
  CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
  dict->SetString("mode", "fixed_servers");
  dict->SetString("server", "socks5://127.0.0.1:" + std::to_string(port));
  dict->SetString("bypass_list", "<-loopback>");
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetDictionary(dict);
  return value;
}

CefRefPtr<CefValue> BoolValue(bool v) {
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetBool(v);
  return value;
}

const char* KindName(ContextManager::Kind kind) {
  switch (kind) {
    case ContextManager::Kind::kInternal:
      return "internal";
    case ContextManager::Kind::kNormal:
      return "normal";
    case ContextManager::Kind::kPrivate:
      return "private";
    case ContextManager::Kind::kTor:
      return "tor";
  }
  return "unknown";
}

}  // namespace

bool SetPrefVerified(CefRefPtr<CefPreferenceManager> context,
                     const std::string& context_name,
                     const std::string& name,
                     CefRefPtr<CefValue> value,
                     std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  CefString err;
  bool ok = context->CanSetPreference(name) && context->SetPreference(name, value, err);
  CefRefPtr<CefValue> readback = ok ? context->GetPreference(name) : nullptr;
  bool verified = ok && readback && readback->IsEqual(value);
  test_log::Event("pref", {{"context", context_name},
                          {"name", name},
                          {"ok", verified},
                          {"value", readback && readback->GetType() == VTYPE_STRING
                                        ? readback->GetString().ToString()
                                        : std::string()}});
  if (!verified) {
    *error = "Could not apply the privacy preference \"" + name + "\" to the " + context_name +
             " context" + (err.empty() ? "" : (": " + err.ToString())) + ".";
  }
  return verified;
}

class ContextManager::Handler : public CefRequestContextHandler {
 public:
  Handler(ContextManager* owner, Kind kind) : owner_(owner), kind_(kind) {}

  // Only reached for requests no browser's client handles (service and
  // shared workers): Normal's get the tracker and ad blocking tabs get.
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override {
    if (kind_ != Kind::kNormal) {
      return nullptr;
    }
    PrivacyService* privacy = GetServices().privacy;
    return new content_filter::ContextRequestHandler(
        request_initiator.ToString(), nullptr, privacy ? privacy->normal_ads_allowed() : nullptr);
  }

  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override {
    CEF_REQUIRE_UI_THREAD();
    if (owner_) {
      owner_->OnInitialized(kind_, context);
      owner_ = nullptr;
    }
  }

 private:
  ContextManager* owner_;
  Kind kind_;
  IMPLEMENT_REFCOUNTING(Handler);
};

// Cancels the Tor context's requests that no tab made, while tor is not
// connected or once the context is retired (New Identity), and any to this
// computer or the local network.
class TorGate : public CefResourceRequestHandler {
 public:
  TorGate(std::function<bool()> allowed, std::shared_ptr<const std::atomic<bool>> retired)
      : allowed_(std::move(allowed)), retired_(std::move(retired)) {}

  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override {
    const std::string url = request->GetURL().ToString();
    std::string why = guards::ResourceBlockReason(guards::Role::kTorTab, url);
    if (why.empty() && retired_ && retired_->load()) {
      why = "tor identity retired";
    }
    if (why.empty() && !fault_injection::SkipTorGuard() && !(allowed_ && allowed_())) {
      why = "tor not connected";
    }
    if (!why.empty()) {
      test_log::Event("tor_blocked", {{"role", "context"}, {"url", url.substr(0, 300)}, {"why", why}});
      return RV_CANCEL;
    }
    return RV_CONTINUE;
  }

 private:
  const std::function<bool()> allowed_;
  const std::shared_ptr<const std::atomic<bool>> retired_;
  IMPLEMENT_REFCOUNTING(TorGate);
};

class ContextManager::PrivateHandler : public CefRequestContextHandler {
 public:
  PrivateHandler(ContextManager* owner, PrivateCallback done, bool tor = false,
                 std::function<bool()> tor_gate = nullptr,
                 std::shared_ptr<const std::atomic<bool>> retired = nullptr, bool web = true)
      : owner_(owner),
        done_(std::move(done)),
        tor_(tor),
        web_(web || tor),
        gate_(tor ? new TorGate(std::move(tor_gate), std::move(retired)) : nullptr) {}

  // Only reached for requests no browser's client handles (CEF asks the
  // browser's client first, and TorFox's tabs always answer): service and
  // shared workers. Tor's gate first, then tracker and ad blocking. Ads
  // allowed on a site in a private or Tor window are allowed for its tabs
  // only: these requests belong to no window, so they are always filtered.
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override {
    if (!web_) {
      return gate_;  // TorFox's own requests (the updater's context).
    }
    return new content_filter::ContextRequestHandler(request_initiator.ToString(), gate_, nullptr);
  }

  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override {
    CEF_REQUIRE_UI_THREAD();
    if (owner_) {
      ContextManager* owner = owner_;
      owner_ = nullptr;
      if (tor_) {
        owner->OnTorInitialized(this, context);
      } else {
        owner->OnPrivateInitialized(this, context);
      }
    }
  }

  // Shutdown(): the owner is gone and |done_| must never run.
  void Detach() {
    owner_ = nullptr;
    done_ = nullptr;
  }

  PrivateCallback TakeCallback() { return std::move(done_); }

 private:
  ContextManager* owner_;
  PrivateCallback done_;
  const bool tor_;
  const bool web_;  // A window's context: its workers' requests are filtered.
  CefRefPtr<TorGate> gate_;
  IMPLEMENT_REFCOUNTING(PrivateHandler);
};

void ContextManager::CreatePrivate(PrivateCallback done, bool web) {
  CEF_REQUIRE_UI_THREAD();
  if (!internal_ || !normal_) {
    done(nullptr, "TorFox is not ready to open a private window.");
    return;
  }
  // An empty cache_path gives a new, unique off-the-record profile: nothing
  // on disk, and no storage shared with any other context.
  CefRequestContextSettings settings;
  settings.persist_session_cookies = false;
  CefRefPtr<PrivateHandler> handler =
      new PrivateHandler(this, std::move(done), false, nullptr, nullptr, web);
  pending_private_[handler.get()] = CefRequestContext::CreateContext(settings, handler);
}

void ContextManager::CreateTor(int socks_port, PrivateCallback done) {
  CEF_REQUIRE_UI_THREAD();
  if (!internal_ || !normal_) {
    done(nullptr, "TorFox is not ready to open a Tor window.");
    return;
  }
  // Without its own placeholder port, the Tor context would have nowhere
  // safe to point while tor is not running: no Tor windows at all.
  if (platform::ReservedLoopbackPort() <= 0) {
    test_log::Event("tor_context_failed", {{"error", "placeholder"}});
    done(nullptr, "TorFox could not reserve the local port Tor windows need. Restart TorFox.");
    return;
  }
  if (socks_port > 0) {
    tor_port_ = socks_port;
  }
  if (tor_) {
    done(tor_, std::string());
    return;
  }
  tor_waiting_.push_back(std::move(done));
  if (tor_pending_) {
    return;
  }
  tor_pending_ = true;
  tor_pending_retired_ = std::make_shared<std::atomic<bool>>(false);
  CefRequestContextSettings settings;
  settings.persist_session_cookies = false;
  CefRefPtr<PrivateHandler> handler =
      new PrivateHandler(this, nullptr, true, tor_gate_, tor_pending_retired_);
  pending_private_[handler.get()] = CefRequestContext::CreateContext(settings, handler);
}

void ContextManager::OnTorInitialized(PrivateHandler* handler,
                                      CefRefPtr<CefRequestContext> context) {
  pending_private_.erase(handler);
  tor_pending_ = false;
  std::vector<PrivateCallback> waiting;
  waiting.swap(tor_waiting_);
  std::string error;
  if (!CheckStorage(Kind::kTor, context, &error) || !ApplyPrefs(Kind::kTor, context, &error)) {
    test_log::Event("tor_context_failed", {{"error", error}});
    for (PrivateCallback& done : waiting) {
      done(nullptr, error);
    }
    return;
  }
  tor_ = context;
  tor_retired_ = std::move(tor_pending_retired_);
  test_log::Event("context_ready", {{"kind", "tor"},
                                    {"socks_port", tor_port_},
                                    {"proxy_port", TorProxyPort()},
                                    {"placeholder", platform::ReservedLoopbackPort()}});
  for (PrivateCallback& done : waiting) {
    done(context, std::string());
  }
}

int ContextManager::TorProxyPort() const {
  return tor_port_ > 0 ? tor_port_ : platform::ReservedLoopbackPort();
}

bool ContextManager::SetTorPort(int socks_port, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  tor_port_ = socks_port > 0 ? socks_port : 0;
  if (!tor_) {
    return true;
  }
  const bool ok = SetPrefVerified(tor_, "tor", "proxy", TorProxyValue(TorProxyPort()), error);
  test_log::Event("tor_proxy_port", {{"port", TorProxyPort()}, {"ok", ok}});
  return ok;
}

bool ContextManager::DropTor(std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  if (!tor_) {
    return true;
  }
  tor_retired_->store(true);
  const bool ok = SetPrefVerified(tor_, "tor (retired)", "proxy",
                                  TorProxyValue(platform::ReservedLoopbackPort()), error);
  test_log::Event("tor_context_retired",
                  {{"proxy_changed", ok}, {"port", platform::ReservedLoopbackPort()}});
  old_tor_.emplace_back(tor_, tor_retired_);
  tor_ = nullptr;
  tor_retired_ = nullptr;
  return ok;
}

std::shared_ptr<const std::atomic<bool>> ContextManager::TorRetired(
    CefRefPtr<CefRequestContext> context) const {
  if (!context) {
    return nullptr;
  }
  if (tor_ && tor_->IsSame(context)) {
    return tor_retired_;
  }
  for (const auto& [old, retired] : old_tor_) {
    if (old->IsSame(context)) {
      return retired;
    }
  }
  return nullptr;
}

bool ContextManager::IsTor(CefRefPtr<CefRequestContext> context) const {
  if (!context) {
    return false;
  }
  if (tor_ && tor_->IsSame(context)) {
    return true;
  }
  for (const auto& [old, retired] : old_tor_) {
    if (old->IsSame(context)) {
      return true;
    }
  }
  return false;
}

void ContextManager::OnPrivateInitialized(PrivateHandler* handler,
                                          CefRefPtr<CefRequestContext> context) {
  pending_private_.erase(handler);
  PrivateCallback done = handler->TakeCallback();
  std::string error;
  if (!CheckStorage(Kind::kPrivate, context, &error) ||
      !ApplyPrefs(Kind::kPrivate, context, &error)) {
    test_log::Event("private_context_failed", {{"error", error}});
    if (done) {
      done(nullptr, error);
    }
    return;
  }
  test_log::Event("context_ready", {{"kind", "private"}});
  if (done) {
    done(context, std::string());
  }
}

void ContextManager::Initialize(const std::filesystem::path& root_cache_path,
                                CefRefPtr<CefSchemeHandlerFactory> internal_scheme_factory,
                                DoneCallback done) {
  CEF_REQUIRE_UI_THREAD();
  scheme_factory_ = internal_scheme_factory;
  done_ = std::move(done);
  pending_ = 2;

  // Internal: off-the-record (empty cache_path) and cookies disabled entirely.
  CefRequestContextSettings internal_settings;
  internal_settings.persist_session_cookies = false;
  internal_settings.cookieable_schemes_exclude_defaults = true;
  internal_ = CefRequestContext::CreateContext(internal_settings,
                                               new Handler(this, Kind::kInternal));

  // Normal: persistent profile. Chrome creates profiles only as direct
  // children of the user data directory (root_cache_path).
  normal_path_ = root_cache_path / "Normal";
  CefRequestContextSettings normal_settings;
  CefString(&normal_settings.cache_path) = platform::PathToUtf8(normal_path_);
  normal_settings.persist_session_cookies = false;
  normal_ = CefRequestContext::CreateContext(normal_settings, new Handler(this, Kind::kNormal));
}

void ContextManager::OnInitialized(Kind kind, CefRefPtr<CefRequestContext> context) {
  if (failed_) {
    return;
  }
  std::string error;
  if (!CheckStorage(kind, context, &error) || !ApplyPrefs(kind, context, &error)) {
    failed_ = true;
    Finish(false, error);
    return;
  }
  if (kind == Kind::kInternal) {
    if (!context->RegisterSchemeHandlerFactory(std::string(kInternalScheme), "",
                                               scheme_factory_)) {
      failed_ = true;
      Finish(false, "Could not register the internal page handler.");
      return;
    }
  }
  test_log::Event("context_ready", {{"kind", KindName(kind)}});
  if (--pending_ == 0) {
    Finish(true, std::string());
  }
}

bool ContextManager::CheckStorage(Kind kind,
                                  CefRefPtr<CefRequestContext> context,
                                  std::string* error) {
  // CEF can hand back a working context even when the profile it was asked
  // for could not be created. Confirm each context is the one requested.
  const std::string actual = context->GetCachePath().ToString();
  bool ok = !context->IsGlobal();
  if (kind == Kind::kInternal || kind == Kind::kPrivate || kind == Kind::kTor) {
    ok = ok && actual.empty();  // Off the record: nothing on disk.
  } else {
    std::error_code ec;
    ok = ok && !actual.empty() &&
         std::filesystem::equivalent(platform::PathFromUtf8(actual), normal_path_, ec) && !ec;
  }
  test_log::Event("context_storage",
                  {{"kind", KindName(kind)}, {"ok", ok}, {"cache_path", actual}});
  if (!ok) {
    *error = std::string("The ") + KindName(kind) +
             " browsing profile could not be created where TorFox expected it.";
  }
  return ok;
}

bool ContextManager::ApplyPrefs(Kind kind, CefRefPtr<CefRequestContext> context,
                                std::string* error) {
  const std::string name = KindName(kind);
  // Each preference below was verified to take effect for Alloy-style
  // browsers in docs/STAGE0_FINDINGS.md (spike D). Every context is set
  // explicitly; values do not propagate between contexts.
  if (!SetPrefVerified(context, name, "net.network_prediction_options", IntValue(2), error)) {
    return false;
  }
  // Third-party cookies: always blocked for internal pages and Tor windows;
  // normal and private windows follow Settings > Privacy & Security.
  const int cookie_mode =
      kind == Kind::kInternal || kind == Kind::kTor || block_third_party_cookies_ ? 1 : 0;
  if (!SetPrefVerified(context, name, "profile.cookie_controls_mode", IntValue(cookie_mode),
                       error)) {
    return false;
  }
  // Tor: SOCKS cannot carry UDP, so WebRTC may use no other route.
  const char* webrtc = kind == Kind::kInternal || kind == Kind::kTor
                           ? "disable_non_proxied_udp"
                           : "default_public_interface_only";
  if (!SetPrefVerified(context, name, "webrtc.ip_handling_policy", StringValue(webrtc), error)) {
    return false;
  }
  // Pop-ups: TorFox's OnBeforePopup decides (Settings > Privacy & Security
  // > Block pop-ups) and opens allowed ones as tabs, never as windows.
  // Chromium's own blocker would refuse gesture-less pop-ups before
  // TorFox sees them, so the setting could not be turned off and the user
  // could not be told (docs/STAGE4_FINDINGS.md).
  if (kind != Kind::kInternal &&
      !SetPrefVerified(context, name, "profile.default_content_setting_values.popups",
                       IntValue(1), error)) {
    return false;
  }
  // TorFox keeps its own history (places.db, Normal windows only). Chrome's
  // history service would otherwise record every visit in <profile>/History,
  // which TorFox's history page could neither show nor clear.
  if (!SetPrefVerified(context, name, "history.saving_disabled", BoolValue(true), error)) {
    return false;
  }
  // Chromium's DNS probe: after a page fails with "name not resolved" it
  // looks up google.com itself, to tell the user whether DNS works. TorFox
  // shows its own error pages and sends nothing it was not asked to
  // (docs/STAGE9_FINDINGS.md). The probe runs only while this is on.
  if (!SetPrefVerified(context, name, "alternate_error_pages.enabled", BoolValue(false), error)) {
    return false;
  }
  // Downloads (web contexts): TorFox chooses every file's path itself
  // (TabClient::OnBeforeDownload), but Chromium writes a download's first
  // bytes to a temporary file in this folder before that. It is TorFox's
  // staging folder, so unconfirmed bytes never land in Downloads.
  if (kind != Kind::kInternal && !download_dir_.empty()) {
    if (!SetPrefVerified(context, name, "download.default_directory",
                         StringValue(platform::PathToUtf8(download_dir_)), error) ||
        !SetPrefVerified(context, name, "download.prompt_for_download", BoolValue(false),
                         error)) {
      return false;
    }
  }
  if (kind != Kind::kInternal &&
      !SetPrefVerified(context, name, "profile.default_content_setting_values.javascript_optimizer",
                       IntValue(JsOptimizerOff(kind) ? 2 : 1), error)) {  // Block : allow.
    return false;
  }
  // Tor: tor's SOCKS port and nothing else.
  if (kind == Kind::kTor) {
    return SetPrefVerified(context, name, "proxy", TorProxyValue(TorProxyPort()), error);
  }
  // Web contexts: direct unless the user chose the system's proxy settings.
  if (kind != Kind::kInternal &&
      !SetPrefVerified(context, name, "proxy",
                       ProxyValue(use_system_proxy_ ? "system" : "direct"), error)) {
    return false;
  }
  // Internal pages never use the network, so their context must not even
  // look for a proxy ("wpad" lookups; see BrowserApp::ApplyGlobalPrefs).
  if (kind == Kind::kInternal &&
      !SetPrefVerified(context, name, "proxy", ProxyValue("direct"), error)) {
    return false;
  }
  return true;
}

bool ContextManager::SetBlockThirdPartyCookies(bool block, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  block_third_party_cookies_ = block;
  return !normal_ || ApplyCookiePolicy(normal_, error);
}

bool ContextManager::ApplyCookiePolicy(CefRefPtr<CefRequestContext> context, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  if (IsTor(context)) {
    return true;  // Always blocked in the Tor context.
  }
  const std::string name = context == normal_ ? "normal" : "private";
  return SetPrefVerified(context, name, "profile.cookie_controls_mode",
                         IntValue(block_third_party_cookies_ ? 1 : 0), error);
}

bool ContextManager::SetUseSystemProxy(bool use_system, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  use_system_proxy_ = use_system;
  return !normal_ || ApplyProxyMode(normal_, error);
}

bool ContextManager::ApplyProxyMode(CefRefPtr<CefRequestContext> context, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  if (IsTor(context)) {
    // Never: the Tor context's proxy is tor's SOCKS port, whatever the
    // system proxy setting says.
    test_log::Event("proxy_mode_refused", {{"context", "tor"}});
    return true;
  }
  const std::string name = context == normal_ ? "normal" : "private";
  return SetPrefVerified(context, name, "proxy",
                         ProxyValue(use_system_proxy_ ? "system" : "direct"), error);
}

bool ContextManager::JsOptimizerOff(Kind kind) const {
  // Anything but "on" keeps it off in Tor windows.
  return kind == Kind::kTor ? js_optimizer_ != "on" : js_optimizer_ == "off";
}

bool ContextManager::SetJsOptimizer(const std::string& mode, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  js_optimizer_ = mode;
  return (!normal_ || ApplyJsOptimizer(normal_, error)) && (!tor_ || ApplyJsOptimizer(tor_, error));
}

bool ContextManager::ApplyJsOptimizer(CefRefPtr<CefRequestContext> context, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  const bool tor = IsTor(context);
  const std::string name = tor ? "tor" : context == normal_ ? "normal" : "private";
  const bool off = JsOptimizerOff(tor ? Kind::kTor : Kind::kNormal);
  const bool ok =
      SetPrefVerified(context, name, "profile.default_content_setting_values.javascript_optimizer",
                      IntValue(off ? 2 : 1), error);
  test_log::Event("js_optimizer", {{"context", name}, {"off", off}, {"ok", ok}});
  return ok;
}

bool ContextManager::ClearSiteRecords(std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  if (!normal_) {
    return true;
  }
  for (const char* pref : {"partition.per_host_zoom_levels",
                           "profile.content_settings.exceptions.site_engagement",
                           "profile.content_settings.exceptions.media_engagement"}) {
    CefRefPtr<CefValue> empty = CefValue::Create();
    empty->SetDictionary(CefDictionaryValue::Create());
    if (!SetPrefVerified(normal_, "normal", pref, empty, error)) {
      return false;
    }
  }
  return true;
}

void ContextManager::Finish(bool ok, const std::string& error) {
  DoneCallback done = std::move(done_);
  done_ = nullptr;
  if (done) {
    done(ok, error);
  }
}

CefRefPtr<CefRequestContext> ContextManager::Get(Kind kind) const {
  switch (kind) {
    case Kind::kInternal:
      return internal_;
    case Kind::kNormal:
      return normal_;
    case Kind::kPrivate:
      break;  // Owned by each private window.
  }
  return nullptr;
}

void ContextManager::Shutdown() {
  if (internal_) {
    internal_->ClearSchemeHandlerFactories();
  }
  internal_ = nullptr;
  normal_ = nullptr;
  tor_ = nullptr;
  old_tor_.clear();
  tor_waiting_.clear();
  for (auto& [handler, context] : pending_private_) {
    handler->Detach();
  }
  pending_private_.clear();
  scheme_factory_ = nullptr;
  done_ = nullptr;
}

}  // namespace torfox
