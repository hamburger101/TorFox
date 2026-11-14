// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_CONTEXT_MANAGER_H_
#define TORFOX_BROWSER_APP_CONTEXT_MANAGER_H_

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "include/cef_preference.h"
#include "include/cef_request_context.h"
#include "include/cef_scheme.h"

namespace torfox {

// Creates and owns TorFox's request contexts (isolation boundaries):
//   Internal: off-the-record; the only context with a torfox:// handler;
//             hosts the shell and internal pages; no web access.
//   Normal:   persistent profile at <root cache>/Normal; web tabs.
//   Private:  one new off-the-record context per private window, created on
//             demand; nothing is written to disk and two private windows
//             share nothing. Owned by its window.
// (The Tor context arrives in Stage 5.)
//
// No browser may be created until Initialize() reports success: privacy
// preferences are applied and read back on every context first, and any
// failure is fatal (fail closed).
class ContextManager {
 public:
  enum class Kind { kInternal, kNormal, kPrivate };

  using DoneCallback = std::function<void(bool ok, const std::string& error)>;
  // |context| is null on failure, with |error| set.
  using PrivateCallback =
      std::function<void(CefRefPtr<CefRequestContext> context, const std::string& error)>;

  // |root_cache_path| must equal CefSettings.root_cache_path.
  void Initialize(const std::filesystem::path& root_cache_path,
                  CefRefPtr<CefSchemeHandlerFactory> internal_scheme_factory,
                  DoneCallback done);

  // kInternal or kNormal.
  CefRefPtr<CefRequestContext> Get(Kind kind) const;

  // Creates a new private context. |done| runs on the UI thread once the
  // context's storage and privacy preferences have been checked; it never
  // runs after Shutdown().
  void CreatePrivate(PrivateCallback done);

  // Drops every reference; required before CefShutdown().
  void Shutdown();

 private:
  class Handler;
  class PrivateHandler;
  void OnInitialized(Kind kind, CefRefPtr<CefRequestContext> context);
  bool CheckStorage(Kind kind, CefRefPtr<CefRequestContext> context, std::string* error);
  bool ApplyPrefs(Kind kind, CefRefPtr<CefRequestContext> context, std::string* error);
  void Finish(bool ok, const std::string& error);
  void OnPrivateInitialized(PrivateHandler* handler, CefRefPtr<CefRequestContext> context);

  CefRefPtr<CefRequestContext> internal_;
  CefRefPtr<CefRequestContext> normal_;
  CefRefPtr<CefSchemeHandlerFactory> scheme_factory_;
  std::filesystem::path normal_path_;
  DoneCallback done_;
  int pending_ = 0;
  bool failed_ = false;
  // Private contexts still initializing, kept alive until they are ready.
  std::map<PrivateHandler*, CefRefPtr<CefRequestContext>> pending_private_;
};

// Sets |name| to |value| on |context| (a request context, or the global
// preference manager for Local State) and reads it back. Returns false (with
// |error| set) if the preference cannot be set or does not stick.
bool SetPrefVerified(CefRefPtr<CefPreferenceManager> context,
                     const std::string& context_name,
                     const std::string& name,
                     CefRefPtr<CefValue> value,
                     std::string* error);

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_CONTEXT_MANAGER_H_
