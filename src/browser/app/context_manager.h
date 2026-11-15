// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_CONTEXT_MANAGER_H_
#define TORFOX_BROWSER_APP_CONTEXT_MANAGER_H_

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
  enum class Kind { kInternal, kNormal, kPrivate, kTor };

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

  // Chromium's default download folder for web contexts, where it writes
  // a download's first bytes before TorFox picks the real path
  // (DownloadService::StagingDirectory()). Call before Initialize().
  void SetDownloadDirectory(std::filesystem::path dir) { download_dir_ = std::move(dir); }

  // The Tor context, shared by every Tor window until New Identity drops
  // it: off the record, and its only route out is tor's SOCKS port
  // (fixed_servers, socks5://127.0.0.1:<port>, never DIRECT, loopback
  // included). Before tor has reported a port it is the placeholder,
  // platform::ReservedLoopbackPort(): TorFox's own, never listening, so a
  // request that got past the Tor windows' guards is refused rather than
  // sent anywhere. Without it there is no Tor context. |done| runs once
  // the context and its preferences have been read back. While the context
  // exists, later calls get the same one.
  void CreateTor(int socks_port, PrivateCallback done);
  // Points the Tor context (if any, and any created later) at |socks_port|,
  // or at the placeholder when it is 0 (tor has stopped).
  bool SetTorPort(int socks_port, std::string* error);
  // Retires the Tor context (New Identity): from now on it sends nothing.
  // The guards of its windows and its own gate cancel every request
  // (TorRetired()), and its proxy points at the placeholder, so even a
  // request no guard sees (a WebSocket) cannot reach tor. Windows that still
  // hold it keep it until they close; the next CreateTor() makes a new one.
  // False, with |error|, if the proxy could not be changed (the context is
  // retired all the same).
  bool DropTor(std::string* error);
  bool IsTor(CefRefPtr<CefRequestContext> context) const;
  // Set once |context|, a Tor context, has been retired by DropTor(); null
  // for any other context. Safe to read on any thread.
  std::shared_ptr<const std::atomic<bool>> TorRetired(CefRefPtr<CefRequestContext> context) const;
  // Requests in the Tor context that no tab makes (service workers, for
  // example) are cancelled unless |allowed| returns true. Called on the IO
  // thread; set before the first Tor context is created.
  void SetTorGate(std::function<bool()> allowed) { tor_gate_ = std::move(allowed); }
  // The Tor context's proxy port: tor's, or the placeholder.
  int TorProxyPort() const;

  // Settings > Privacy & Security > Block third-party cookies, for web
  // contexts: applied to Normal now (if it exists) and to every context
  // created later. Private windows' contexts belong to the windows; apply it
  // to them with ApplyCookiePolicy. Internal pages always block them.
  // Returns false, with |error|, if the preference did not take effect.
  bool SetBlockThirdPartyCookies(bool block, std::string* error);
  bool ApplyCookiePolicy(CefRefPtr<CefRequestContext> context, std::string* error);

  // Settings > Privacy & Security > Use the system's proxy settings, for
  // web contexts ("system" or "direct"), the same way.
  bool SetUseSystemProxy(bool use_system, std::string* error);
  bool ApplyProxyMode(CefRefPtr<CefRequestContext> context, std::string* error);

  // Settings > Privacy & Security > JavaScript optimizer ("off-in-tor",
  // "on" or "off"): Chromium's per-site V8 optimizer setting, for every web
  // context. Where it is off, each site's renderer process is started with
  // --js-flags=--disable-optimizing-compilers, so it applies to pages opened from then
  // on. Set on Normal and Tor now and on contexts created later; apply it
  // to private windows' contexts with ApplyJsOptimizer.
  bool SetJsOptimizer(const std::string& mode, std::string* error);
  bool ApplyJsOptimizer(CefRefPtr<CefRequestContext> context, std::string* error);

  // History > Clear history: Chromium's per-site zoom levels and scores in
  // the Normal profile (chromium_profile::ScrubSiteRecords does the same
  // while Chromium is not running). Open pages keep their zoom until
  // TorFox closes. False, with |error|, if a preference did not take effect.
  bool ClearSiteRecords(std::string* error);

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
  void OnTorInitialized(PrivateHandler* handler, CefRefPtr<CefRequestContext> context);

  CefRefPtr<CefRequestContext> internal_;
  CefRefPtr<CefRequestContext> normal_;
  CefRefPtr<CefRequestContext> tor_;
  using RetiredFlag = std::shared_ptr<std::atomic<bool>>;
  RetiredFlag tor_retired_;          // tor_'s.
  RetiredFlag tor_pending_retired_;  // The Tor context being created.
  // Retired by DropTor(), possibly still in use.
  std::vector<std::pair<CefRefPtr<CefRequestContext>, RetiredFlag>> old_tor_;
  int tor_port_ = 0;  // tor's SOCKS port; 0 while tor has none.
  bool tor_pending_ = false;
  std::vector<PrivateCallback> tor_waiting_;
  std::function<bool()> tor_gate_;
  CefRefPtr<CefSchemeHandlerFactory> scheme_factory_;
  std::filesystem::path normal_path_;
  std::filesystem::path download_dir_;
  bool block_third_party_cookies_ = true;
  bool use_system_proxy_ = false;
  std::string js_optimizer_ = "off-in-tor";
  bool JsOptimizerOff(Kind kind) const;
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
