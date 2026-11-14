// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/context_manager.h"

#include <system_error>

#include "include/cef_request_context_handler.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"
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

CefRefPtr<CefValue> DirectProxyValue() {
  CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
  dict->SetString("mode", "direct");
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
  test_log::Event("pref", {{"context", context_name}, {"name", name}, {"ok", verified}});
  if (!verified) {
    *error = "Could not apply the privacy preference \"" + name + "\" to the " + context_name +
             " context" + (err.empty() ? "" : (": " + err.ToString())) + ".";
  }
  return verified;
}

class ContextManager::Handler : public CefRequestContextHandler {
 public:
  Handler(ContextManager* owner, Kind kind) : owner_(owner), kind_(kind) {}

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

class ContextManager::PrivateHandler : public CefRequestContextHandler {
 public:
  PrivateHandler(ContextManager* owner, PrivateCallback done)
      : owner_(owner), done_(std::move(done)) {}

  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override {
    CEF_REQUIRE_UI_THREAD();
    if (owner_) {
      ContextManager* owner = owner_;
      owner_ = nullptr;
      owner->OnPrivateInitialized(this, context);
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
  IMPLEMENT_REFCOUNTING(PrivateHandler);
};

void ContextManager::CreatePrivate(PrivateCallback done) {
  CEF_REQUIRE_UI_THREAD();
  if (!internal_ || !normal_) {
    done(nullptr, "TorFox is not ready to open a private window.");
    return;
  }
  // An empty cache_path gives a new, unique off-the-record profile: nothing
  // on disk, and no storage shared with any other context.
  CefRequestContextSettings settings;
  settings.persist_session_cookies = false;
  CefRefPtr<PrivateHandler> handler = new PrivateHandler(this, std::move(done));
  pending_private_[handler.get()] = CefRequestContext::CreateContext(settings, handler);
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
  if (kind == Kind::kInternal || kind == Kind::kPrivate) {
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
  if (!SetPrefVerified(context, name, "profile.cookie_controls_mode", IntValue(1), error)) {
    return false;
  }
  const char* webrtc = kind == Kind::kInternal ? "disable_non_proxied_udp"
                                               : "default_public_interface_only";
  if (!SetPrefVerified(context, name, "webrtc.ip_handling_policy", StringValue(webrtc), error)) {
    return false;
  }
  // TorFox keeps its own history (places.db, Normal windows only). Chrome's
  // history service would otherwise record every visit in <profile>/History,
  // which TorFox's history page could neither show nor clear.
  if (!SetPrefVerified(context, name, "history.saving_disabled", BoolValue(true), error)) {
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
  // Internal pages never use the network, so their context must not even
  // look for a proxy ("wpad" lookups; see BrowserApp::ApplyGlobalPrefs).
  if (kind == Kind::kInternal &&
      !SetPrefVerified(context, name, "proxy", DirectProxyValue(), error)) {
    return false;
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
  for (auto& [handler, context] : pending_private_) {
    handler->Detach();
  }
  pending_private_.clear();
  scheme_factory_ = nullptr;
  done_ = nullptr;
}

}  // namespace torfox
