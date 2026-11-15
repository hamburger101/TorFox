// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/browser_app.h"

#include <algorithm>
#include <system_error>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_request_context.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/app_common.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/download_service.h"
#include "src/core/browsing/chromium_profile.h"
#include "src/browser/app/renderer_app.h"
#include "src/browser/app/services.h"
#include "src/browser/app/release_config.h"
#include "src/browser/app/session_service.h"
#include "src/browser/app/update_service.h"
#include "src/browser/app/version.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/places_service.h"
#include "src/browser/app/privacy_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/app/tor_service.h"
#include "src/browser/bridge/bridge_handler.h"
#include "src/browser/platform/platform.h"
#include "src/browser/scheme/internal_scheme.h"
#include "src/browser/shell/browser_window.h"
#include "src/browser/shell/dialogs.h"
#include "src/browser/shell/menus.h"
#include "src/core/cmdline/policy.h"
#include "src/core/settings/store.h"
#include "src/core/update/whats_new.h"
#include "src/core/url/search.h"
#include "src/core/util/file_io.h"

namespace torfox {

namespace {

Services g_services;

// In the profile folder: the version of the update TorFox last handed to its
// setup (update::CheckHandoff).
constexpr char kUpdateHandoffFile[] = "update-handoff";

cmdline::SwitchMap SwitchesOf(CefRefPtr<CefCommandLine> cl) {
  CefCommandLine::SwitchMap raw;
  cl->GetSwitches(raw);
  cmdline::SwitchMap out;
  for (const auto& [name, value] : raw) {
    out[name.ToString()] = value.ToString();
  }
  return out;
}

std::vector<std::string> ArgsOf(CefRefPtr<CefCommandLine> cl) {
  CefCommandLine::ArgumentList raw;
  cl->GetArguments(raw);
  std::vector<std::string> out;
  for (const CefString& a : raw) {
    out.push_back(a.ToString());
  }
  return out;
}

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

CefRefPtr<CefValue> BoolValue(bool v) {
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetBool(v);
  return value;
}

}  // namespace

std::string SearchUrlFor(const std::string& search_url, bool tor) {
  Services& services = GetServices();
  const std::string engine =
      services.settings ? services.settings->store().GetString(search::kEngineSetting) : "";
  return search::ForEngine(search_url, engine, tor);
}

Services& GetServices() {
  return g_services;
}

BrowserApp::BrowserApp(StartupOptions options) : options_(std::move(options)) {}

BrowserApp::~BrowserApp() = default;

void BrowserApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterTorFoxSchemes(registrar);
}

void BrowserApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                               CefRefPtr<CefCommandLine> command_line) {
  if (!process_type.empty()) {
    return;
  }
  CefRefPtr<CefCommandLine> raw = platform::RawCommandLine();
  cmdline::Input input;
  input.user_switches = SwitchesOf(raw);
  input.user_args = ArgsOf(raw);
  input.current_switches = SwitchesOf(command_line);
  input.test_hooks = kTestHooks;
  cmdline::Plan plan = cmdline::Apply(input);

  command_line->Reset();
  for (const auto& [name, value] : plan.switches) {
    if (value.empty()) {
      command_line->AppendSwitch(name);
    } else {
      command_line->AppendSwitchWithValue(name, value);
    }
  }
  for (const std::string& arg : plan.args) {
    command_line->AppendArgument(arg);
  }

  Json removed = Json::array();
  for (const std::string& r : plan.removed) {
    removed.push_back(r);
  }
  test_log::Event("command_line", {{"final", command_line->GetCommandLineString().ToString()},
                                   {"removed", removed},
                                   {"dropped_args", plan.dropped_args.size()}});
}

namespace {

CefRefPtr<CefValue> DirectProxy() {
  CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
  dict->SetString("mode", "direct");
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetDictionary(dict);
  return value;
}

// Settings > Privacy & Security > Clear when TorFox closes, read straight
// from the settings file (Chromium is not running, and neither is the
// settings service). A corrupt file is left for the settings service.
// Settings > Privacy & Security > Use the system's proxy settings.
bool UseSystemProxySetting(const std::filesystem::path& user_data_dir) {
  settings::Store store(user_data_dir / "settings.json");
  store.Load(settings::LoadMode::kReadOnly);
  return store.GetBool("privacy.useSystemProxy");
}

clear_data::Categories ClearOnExitSetting(const std::filesystem::path& user_data_dir) {
  settings::Store store(user_data_dir / "settings.json");
  store.Load(settings::LoadMode::kReadOnly);
  clear_data::Categories c;
  c.history = store.GetBool("privacy.clearOnExit.history");
  c.cookies = store.GetBool("privacy.clearOnExit.cookies");
  c.cache = store.GetBool("privacy.clearOnExit.cache");
  return c;
}

bool AllDigits(const std::string& s) {
  return !s.empty() && s.size() <= 5 &&
         std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

bool BrowserApp::ApplyGlobalPrefs(std::string* error) {
  // The global context hosts no content, but off-the-record contexts read
  // through to it for any preference they do not set themselves.
  CefRefPtr<CefRequestContext> global = CefRequestContext::GetGlobalContext();
  // Local State configures the system network context, which Chromium's own
  // services use (TorFox has disabled or cut off every one of them). It must
  // never look for a proxy: with Windows' "Automatically detect settings" on,
  // that is a "wpad" lookup on the local network (seen by the quiet-startup
  // test), and WPAD answers can be forged by anyone on that network.
  CefRefPtr<CefPreferenceManager> local_state = CefPreferenceManager::GetGlobalPreferenceManager();
  return SetPrefVerified(global, "global", "net.network_prediction_options", IntValue(2), error) &&
         SetPrefVerified(global, "global", "profile.cookie_controls_mode", IntValue(1), error) &&
         SetPrefVerified(global, "global", "webrtc.ip_handling_policy",
                         StringValue("disable_non_proxied_udp"), error) &&
         SetPrefVerified(global, "global", "history.saving_disabled", BoolValue(true), error) &&
         SetPrefVerified(global, "global", "proxy", DirectProxy(), error) &&
         SetPrefVerified(local_state, "local_state", "proxy", DirectProxy(), error);
}

void BrowserApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();
  test_log::Event("context_initialized");

  std::string error;
  if (!ApplyGlobalPrefs(&error)) {
    Fatal(error);
    return;
  }

  settings_ = std::make_unique<SettingsService>(options_.user_data_dir / "settings.json");
  settings_->Load();

  g_services.settings = settings_.get();

  // History and bookmarks (Normal windows' history only). Not a security
  // dependency: if it cannot be opened, TorFox runs without it and says so.
  places_ = std::make_unique<PlacesService>(options_.user_data_dir / "places.db");
  places_->Open();
  g_services.places = places_.get();
  // Clear when TorFox closes > History, if the last session could not do
  // it (it ended in a crash).
  if (settings_->store().GetBool("privacy.clearOnExit.history") && places_->places()) {
    test_log::Event("history_cleared", {{"when", "startup"},
                                        {"ok", places_->places()->ClearHistory()}});
  }

  // Downloads: the policy from Settings > Downloads and the in-memory list.
  downloads_ = std::make_unique<DownloadService>(settings_.get(), options_.download_dir,
                                                 options_.user_data_dir / "Pending Downloads");
  downloads_->PrepareStaging();
  g_services.downloads = downloads_.get();

  // Settings > Privacy & Security.
  privacy_ = std::make_unique<PrivacyService>(settings_.get(), options_.https_test_ports);
  g_services.privacy = privacy_.get();
  g_services.startup_notice = startup_notice_;
  g_services.whats_new_from = whats_new_from_;
  g_services.whats_new_verified = whats_new_verified_;
  g_services.show_whats_new = !whats_new_from_.empty();
  g_services.window_opened = [this](BrowserWindow* w) { windows_.insert(w); };
  g_services.window_destroyed = [this](BrowserWindow* w) {
    windows_.erase(w);
    if (!w->is_private() && session_) {
      session_->Changed();
    }
    if (windows_.empty()) {
      CefQuitMessageLoop();
    }
  };

  // Settings > General > On startup: the normal windows' tabs, in the order
  // the windows opened.
  session_ = std::make_unique<SessionService>(
      settings_.get(), options_.user_data_dir / "session.json", [this] {
        std::vector<BrowserWindow*> normal;
        for (BrowserWindow* w : windows_) {
          if (!w->is_private() && !w->closing()) {
            normal.push_back(w);
          }
        }
        std::sort(normal.begin(), normal.end(),
                  [](BrowserWindow* a, BrowserWindow* b) { return a->id() < b->id(); });
        browsing::Session session;
        for (BrowserWindow* w : normal) {
          session.push_back(w->SessionTabs());
        }
        return session;
      });
  g_services.session_changed = [this] {
    if (session_) {
      session_->Changed();
    }
  };

  router_ = CefMessageRouterBrowserSide::Create(BridgeRouterConfig());
  bridge_ = std::make_unique<BridgeHandler>();
  router_->AddHandler(bridge_.get(), /*first=*/false);
  g_services.router = router_;

  contexts_ = std::make_unique<ContextManager>();
  contexts_->SetDownloadDirectory(downloads_->StagingDirectory());
  contexts_->SetBlockThirdPartyCookies(privacy_->block_third_party_cookies(), &error);
  contexts_->SetUseSystemProxy(settings_->store().GetBool("privacy.useSystemProxy"), &error);
  contexts_->SetJsOptimizer(settings_->store().GetString("privacy.jsOptimizer"), &error);
  proxy_observer_ = settings_->AddObserver([this](const std::string& key) {
    if (key == "privacy.useSystemProxy") {
      ApplyProxyMode(settings_->store().GetBool(key));
    }
    if (key == "privacy.jsOptimizer") {
      ApplyJsOptimizer(settings_->store().GetString(key));
    }
  });
  privacy_->SetCookiePolicyListener([this](bool block) { ApplyCookiePolicy(block); });
  g_services.contexts = contexts_.get();

  // Tor. Nothing runs until a Tor window opens (or at launch, if Settings >
  // Tor says so).
  {
    TorService::Options tor_options;
    tor_options.state_dir = options_.user_data_dir / "Tor";
    tor_options.paths = platform::BundledTorPaths();
    tor_options.test_exe = options_.tor_test_exe;
    tor_options.test_args = options_.tor_test_args;
    tor_options.notice_log = kTestHooks;
    tor_ = std::make_shared<TorService>(tor_options, [this](int port) {
      std::string err;
      const bool ok = contexts_ && contexts_->SetTorPort(port, &err);
      test_log::Event("tor_port_applied", {{"port", port}, {"ok", ok}, {"error", err}});
      return ok;
    });
    std::weak_ptr<TorService> weak_tor = tor_;
    contexts_->SetTorGate([weak_tor] {
      std::shared_ptr<TorService> t = weak_tor.lock();
      return t && t->connected();
    });
    g_services.tor = tor_;
    g_services.tor_start = [this] { StartTor(); };
    g_services.tor_new_identity = [this](BrowserWindow* w) { NewTorIdentity(w); };
    tor_settings_observer_ = settings_->AddObserver([this](const std::string& key) {
      if (key.rfind("tor.bridges.", 0) == 0) {
        RestartTorIfRunning();
      }
    });
  }
  CefRefPtr<BrowserApp> self = this;
  contexts_->Initialize(options_.user_data_dir / "User Data", CreateInternalSchemeFactory(),
                        [self](bool ok, const std::string& err) {
                          if (!ok) {
                            self->Fatal(err);
                            return;
                          }
                          browsing::Session restored =
                              self->session_ ? self->session_->TakeStartupSession()
                                             : browsing::Session();
                          if (restored.empty()) {
                            BrowserWindow::Open(self->options_.start_url);
                          } else {
                            std::size_t tabs = 0;
                            for (std::size_t i = 0; i < restored.size(); ++i) {
                              tabs += restored[i].urls.size();
                              // An address TorFox was started with opens
                              // after the first window's tabs.
                              BrowserWindow::OpenRestored(
                                  std::move(restored[i].urls), restored[i].active,
                                  i == 0 ? self->options_.start_url : std::string());
                            }
                            test_log::Event("session_restored",
                                            {{"windows", restored.size()}, {"tabs", tabs}});
                          }
                          if (self->settings_ &&
                              self->settings_->store().GetString("tor.startMode") == "at-launch") {
                            self->StartTor();
                          }
                          self->StartUpdates();
                        });
}

void BrowserApp::StartUpdates() {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !settings_ || !contexts_) {
    return;
  }
  UpdateService::Config config;
  config.repository = kUpdateRepository;
  config.public_key = kUpdatePublicKey;
  config.platform = platform::UpdatePlatform();
  config.installed_version = kTorFoxVersion;
  if (kTestHooks && options_.update_test) {
    // A local fake GitHub (tests/e2e/test_updates.py). Release builds never
    // read this: kTestHooks is false and the switch is not parsed.
    const UpdateTestConfig& t = *options_.update_test;
    config.repository = t.repository;
    config.public_key = t.public_key;
    config.endpoints.api_origin = t.origin;
    config.endpoints.download_origin = t.origin;
    config.endpoints.allowed_origins = {t.origin};
    for (const std::string& o : t.extra_origins) {
      config.endpoints.allowed_origins.push_back(o);
    }
    config.first_check_delay_ms = t.first_check_delay_ms;
  }
  updates_ = std::make_unique<UpdateService>(settings_.get(), contexts_.get(),
                                             options_.user_data_dir, std::move(config));
  updates_->notify = [this](const std::string& version) {
    for (BrowserWindow* w : windows_) {
      if (!w->is_private() && !w->is_tor() && !w->closing()) {
        test_log::Event("update_notice", {{"version", version}, {"window", w->id()}});
        w->ShowNotice("TorFox " + version +
                      " is available. Open Settings, then Updates, to install it.");
        return;
      }
    }
  };
  // The toolbar's "Relaunch to update" follows the updater.
  updates_observer_ = updates_->AddObserver([this] {
    for (BrowserWindow* w : windows_) {
      w->UpdateStatusChanged();
    }
  });
  g_services.updates = updates_.get();
  g_services.close_all_windows = [this] { CloseAllWindows(); };
  g_services.collect_renderers = [this] {
    std::vector<memory::Renderer> out;
    for (BrowserWindow* w : windows_) {
      w->CollectRenderers(&out);
    }
    return out;
  };
  g_services.relaunch_to_update = [this](BrowserWindow* requester, int tab_id,
                                         std::function<void(bool, const std::string&)> done) {
    RelaunchToUpdate(requester, tab_id, std::move(done));
  };
  updates_->Start();
}

void BrowserApp::RelaunchToUpdate(BrowserWindow* requester, int tab_id,
                                  std::function<void(bool, const std::string&)> done) {
  CEF_REQUIRE_UI_THREAD();
  if (!updates_ || updates_->status().state != UpdateService::State::kReady) {
    done(false, "No update is ready to install.");
    return;
  }
  CefRefPtr<BrowserApp> self = this;
  auto relaunch = [self](std::function<void(bool, const std::string&)> reply) {
    std::string error;
    if (!self->updates_ || !self->updates_->PrepareInstall(/*relaunch=*/true, &error)) {
      reply(false, error.empty() ? "TorFox is closing." : error);
      return;
    }
    if (self->session_) {
      self->session_->PrepareRelaunch();
    }
    reply(true, std::string());
    // TorFox closes; the update starts once it has shut down.
    CefPostTask(TID_UI, base::BindOnce([](CefRefPtr<BrowserApp> app) { app->CloseAllWindows(); },
                                       self));
  };
  // Private and Tor windows do not come back after the relaunch: ask first.
  int others = 0;
  for (BrowserWindow* w : windows_) {
    if (w->is_private() && !w->closing()) {
      ++others;
    }
  }
  if (others == 0 || !requester) {
    relaunch(std::move(done));
    return;
  }
  // What actually happens: PrepareRelaunch keeps nothing while history is
  // cleared on exit, and on macOS TorFox only opens the disk image.
  const bool restarts = platform::InstallsUpdatesOnExit();
  const bool keeps_tabs =
      !(settings_ && settings_->store().GetBool("privacy.clearOnExit.history"));
  const std::string version = updates_->status().version;
  platform::ConfirmPrompt prompt;
  prompt.title = restarts ? "Relaunch to update?" : "Close TorFox to update?";
  prompt.heading = others == 1 ? "Your private or Tor window will close"
                               : "Your " + std::to_string(others) +
                                     " private and Tor windows will close";
  prompt.details =
      (restarts ? "TorFox restarts to install version " + version + ". "
                : "TorFox closes and opens the disk image of version " + version +
                      "; copy TorFox to Applications, then open it. ") +
      (keeps_tabs ? "Your normal windows open again; private and Tor tabs do not."
                  : "No tabs open again: TorFox clears your history when it closes.");
  prompt.accept = restarts ? "Relaunch" : "Close TorFox";
  prompt.cancel = "Not now";
  requester->ConfirmForTab(
      tab_id, "update_relaunch", prompt,
      [relaunch, done = std::move(done)](bool ok) mutable {
        if (!ok) {
          done(false, "cancelled");
          return;
        }
        relaunch(std::move(done));
      });
}

void BrowserApp::CloseAllWindows() {
  CEF_REQUIRE_UI_THREAD();
  std::vector<CefRefPtr<BrowserWindow>> all;
  for (BrowserWindow* w : windows_) {
    all.emplace_back(w);
  }
  for (const CefRefPtr<BrowserWindow>& w : all) {
    w->Close();
  }
}

namespace {

TorService::BridgeConfig BridgeSettings(const settings::Store& store) {
  TorService::BridgeConfig c;
  c.mode = store.GetString("tor.bridges.mode");
  c.builtin_type = store.GetString("tor.bridges.builtinType");
  c.custom = store.GetString("tor.bridges.custom");
  return c;
}

}  // namespace

void BrowserApp::StartTor() {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !tor_ || !settings_) {
    return;
  }
  tor_->Start(BridgeSettings(settings_->store()));
}

void BrowserApp::RestartTorIfRunning() {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !tor_ || !settings_ || tor_->status().state == TorService::State::kOff) {
    return;
  }
  test_log::Event("tor_bridges_changed");
  tor_->Restart(BridgeSettings(settings_->store()));
}

void BrowserApp::NewTorIdentity(BrowserWindow* requester) {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !contexts_ || !tor_) {
    return;
  }
  // The Tor windows open now share the Tor context (cookies, cache, open
  // connections): all of them close. The next Tor window gets a new context,
  // and tor builds new circuits for it.
  std::vector<CefRefPtr<BrowserWindow>> old;
  for (BrowserWindow* w : windows_) {
    if (w->is_tor()) {
      old.emplace_back(w);
    }
  }
  // The old context is retired first: from here on it sends nothing (its
  // windows' guards and gate refuse every request and its proxy is a dead
  // port), so nothing from the old identity can use a circuit built after
  // NEWNYM, even while its windows are still closing.
  std::string error;
  if (!contexts_->DropTor(&error)) {
    LOG(ERROR) << "New identity: the old Tor context's proxy was not changed: " << error;
  }
  tor_->NewIdentity([](bool ok) { test_log::Event("tor_newnym", {{"ok", ok}}); });
  // The new window opens first, so closing the old ones never leaves TorFox
  // without a window (which would quit).
  BrowserWindow::OpenTor("", requester && !requester->is_tor() ? CefRefPtr<BrowserWindow>(requester)
                                                              : nullptr,
                         [old](CefRefPtr<BrowserWindow> created) {
                           test_log::Event("tor_new_identity", {{"closed", old.size()},
                                                                {"opened", created != nullptr}});
                           for (const CefRefPtr<BrowserWindow>& w : old) {
                             w->Close();
                           }
                         });
}

bool BrowserApp::OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                             const CefString& current_directory) {
  // A second launch with the same profile: open a new window instead of a
  // second browser process. Only an http(s) URL argument is honoured.
  if (fatal_ || !contexts_) {
    return true;
  }
  std::string url;
  for (const std::string& arg : ArgsOf(command_line)) {
    if (cmdline::IsAbsoluteHttpUrl(arg)) {
      url = arg;
      break;
    }
  }
  BrowserWindow::Open(url);
  return true;
}

void BrowserApp::ApplyCookiePolicy(bool block) {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !contexts_) {
    return;
  }
  std::string error;
  bool ok = contexts_->SetBlockThirdPartyCookies(block, &error);
  for (BrowserWindow* window : windows_) {
    if (ok && window->is_private() && window->web_context()) {
      ok = contexts_->ApplyCookiePolicy(window->web_context(), &error);
    }
  }
  test_log::Event("cookie_policy", {{"block", block}, {"ok", ok}});
  if (!ok) {
    // The setting says one thing and Chromium does another: stop.
    Fatal(error);
  }
}

void BrowserApp::ApplyProxyMode(bool use_system) {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !contexts_) {
    return;
  }
  std::string error;
  bool ok = contexts_->SetUseSystemProxy(use_system, &error);
  for (BrowserWindow* window : windows_) {
    if (ok && window->is_private() && window->web_context()) {
      ok = contexts_->ApplyProxyMode(window->web_context(), &error);
    }
  }
  test_log::Event("proxy_mode", {{"system", use_system}, {"ok", ok}});
  if (!ok) {
    Fatal(error);
  }
}

void BrowserApp::ApplyJsOptimizer(const std::string& mode) {
  CEF_REQUIRE_UI_THREAD();
  if (fatal_ || !contexts_) {
    return;
  }
  std::string error;
  bool ok = contexts_->SetJsOptimizer(mode, &error);
  for (BrowserWindow* window : windows_) {
    // Private windows' own contexts (the Tor context is done above).
    if (ok && window->is_private() && window->web_context() &&
        !contexts_->IsTor(window->web_context())) {
      ok = contexts_->ApplyJsOptimizer(window->web_context(), &error);
    }
  }
  if (!ok) {
    Fatal(error);
  }
}

void BrowserApp::Fatal(const std::string& message) {
  if (fatal_) {
    return;
  }
  fatal_ = true;
  test_log::Event("fatal", {{"message", message}});
  if (!kTestHooks) {
    platform::ShowFatalError("TorFox",
                             "TorFox stopped because a privacy or security check failed:\n\n" +
                                 message);
  }
  CefQuitMessageLoop();
}

void BrowserApp::Shutdown() {
  if (router_ && bridge_) {
    router_->RemoveHandler(bridge_.get());
  }
  if (bridge_) {
    bridge_->Shutdown();
  }
  bridge_.reset();
  router_ = nullptr;
  // tor first: it stops before the Tor context's network goes away.
  if (settings_ && tor_settings_observer_) {
    settings_->RemoveObserver(tor_settings_observer_);
    tor_settings_observer_ = 0;
  }
  if (tor_) {
    tor_->Shutdown();
  }
  // The updater's requests and context go before the contexts do.
  if (updates_) {
    if (updates_observer_) {
      updates_->RemoveObserver(updates_observer_);
      updates_observer_ = 0;
    }
    // Closed with an update waiting: it installs now, like Chrome's, unless
    // TorFox stopped because a check failed or Windows is ending the session
    // (the installer could be cut off). It is downloaded again later.
    if (platform::InstallsUpdatesOnExit() &&
        updates_->status().state == UpdateService::State::kReady) {
      const bool ending = platform::SessionEnding() || (kTestHooks && options_.test_session_ending);
      if (fatal_ || ending) {
        test_log::Event("update_install_skipped", {{"why", fatal_ ? "fatal" : "session_ending"}});
      } else {
        updates_->PrepareInstallOnExit();
      }
    }
    if (std::optional<UpdateService::Handoff> h = updates_->TakeHandoff()) {
      update_handoff_ = UpdateHandoff{h->file, h->sha256, h->size, h->relaunch, h->version};
      test_log::Event("update_handoff", {{"file", h->file.filename().string()},
                                         {"sha256", h->sha256},
                                         {"relaunch", h->relaunch}});
    }
    updates_->Shutdown();
  }
  if (contexts_) {
    contexts_->Shutdown();
  }
  contexts_.reset();
  g_services = Services();
  if (settings_) {
    exit_clear_.history = settings_->store().GetBool("privacy.clearOnExit.history");
    exit_clear_.cookies = settings_->store().GetBool("privacy.clearOnExit.cookies");
    exit_clear_.cache = settings_->store().GetBool("privacy.clearOnExit.cache");
  }
  if (session_) {
    session_->Shutdown();
  }
  if (exit_clear_.history && places_ && places_->places()) {
    test_log::Event("history_cleared", {{"when", "exit"},
                                        {"ok", places_->places()->ClearHistory()}});
  }
  if (settings_ && proxy_observer_) {
    settings_->RemoveObserver(proxy_observer_);
    proxy_observer_ = 0;
  }
  tor_.reset();
  updates_.reset();
  session_.reset();
  privacy_.reset();
  downloads_.reset();
  places_.reset();
  settings_.reset();
  windows_.clear();
  test_log::Event("shutdown");
  test_log::Close();
}

// ----- Process entry ---------------------------------------------------------

int TorFoxMain(const CefMainArgs& args, void* sandbox_info) {
  CefRefPtr<CefCommandLine> raw = platform::RawCommandLine();
  const std::string type = raw->GetSwitchValue("type").ToString();

  CefRefPtr<CefApp> app;
  CefRefPtr<BrowserApp> browser_app;
  StartupOptions options;
  if (type.empty()) {
    options.user_data_dir = platform::DefaultUserDataDir();
    if (kTestHooks) {
      const std::string dir = raw->GetSwitchValue("torfox-test-user-data").ToString();
      if (!dir.empty()) {
        options.user_data_dir = platform::PathFromUtf8(dir);
      }
      test_log::Open(raw->GetSwitchValue("torfox-test-log").ToString());
      test_log::InstallCrashTrace();
      fault_injection::Configure(raw->GetSwitchValue("torfox-test-skip-guards").ToString());
      menus::ConfigureTestChoice(raw->GetSwitchValue("torfox-test-menu-choice").ToString());
      dialogs::ConfigureTestChoice(raw->GetSwitchValue("torfox-test-dialog-choice").ToString());
      const std::string download_dir =
          raw->GetSwitchValue("torfox-test-download-dir").ToString();
      if (!download_dir.empty()) {
        options.download_dir = platform::PathFromUtf8(download_dir);
      }
      const std::string ports = raw->GetSwitchValue("torfox-test-https-port").ToString();
      if (const std::size_t colon = ports.find(':'); colon != std::string::npos) {
        https_first::TestPorts p{ports.substr(0, colon), ports.substr(colon + 1)};
        if (AllDigits(p.http) && AllDigits(p.https)) {
          options.https_test_ports = std::move(p);
        }
      }
      const std::string tor_command = raw->GetSwitchValue("torfox-test-tor-command").ToString();
      if (!tor_command.empty()) {
        const ReadResult r = ReadFileLimited(platform::PathFromUtf8(tor_command), 64 * 1024);
        std::optional<Json> j =
            r.status == ReadStatus::kOk ? ParseJson(r.data, 64 * 1024, 4) : std::nullopt;
        if (j && j->is_object()) {
          if (std::optional<std::string> exe = GetString(*j, "exe")) {
            options.tor_test_exe = platform::PathFromUtf8(*exe);
          }
          if (j->contains("args") && (*j)["args"].is_array()) {
            for (const Json& a : (*j)["args"]) {
              if (a.is_string()) {
                options.tor_test_args.push_back(a.get<std::string>());
              }
            }
          }
        }
        test_log::Event("tor_test_command", {{"exe", platform::PathToUtf8(options.tor_test_exe)},
                                             {"args", options.tor_test_args.size()}});
      }
      const std::string update_config =
          raw->GetSwitchValue("torfox-test-update-config").ToString();
      if (!update_config.empty()) {
        const ReadResult r = ReadFileLimited(platform::PathFromUtf8(update_config), 64 * 1024);
        std::optional<Json> j =
            r.status == ReadStatus::kOk ? ParseJson(r.data, 64 * 1024, 4) : std::nullopt;
        if (j && j->is_object()) {
          UpdateTestConfig t;
          t.origin = GetString(*j, "origin").value_or("");
          t.public_key = GetString(*j, "publicKey").value_or("");
          t.repository = GetString(*j, "repository").value_or("");
          t.first_check_delay_ms =
              static_cast<int>(std::clamp<long long>(GetInt(*j, "firstCheckDelayMs").value_or(0), 0,
                                                     600000));
          if (const Json* extra = GetMember(*j, "extraOrigins"); extra && extra->is_array()) {
            for (const Json& o : *extra) {
              if (o.is_string()) {
                t.extra_origins.push_back(o.get<std::string>());
              }
            }
          }
          options.update_test = std::move(t);
        }
        test_log::Event("update_test_config", {{"ok", options.update_test.has_value()}});
      }
      const std::string start = raw->GetSwitchValue("torfox-test-start-url").ToString();
      if (cmdline::IsAbsoluteHttpUrl(start)) {
        options.start_url = start;
      }
      options.test_session_ending = raw->HasSwitch("torfox-test-session-ending");
    }
    if (options.start_url.empty()) {
      for (const std::string& arg : ArgsOf(raw)) {
        if (cmdline::IsAbsoluteHttpUrl(arg)) {
          options.start_url = arg;
          break;
        }
      }
    }
    // TorFox was closed with an update waiting and its setup is installing
    // it now: starting would hold files the setup has to replace. The setup
    // starts TorFox when it is done instead.
    if (platform::UpdateBeingInstalled()) {
      const bool asked = platform::RequestStartAfterUpdate();
      // The setup lets go of the mutex before it reads the request
      // (TorFox.iss): if the mutex is still held now, the setup will see the
      // request. If it was let go of in between, the setup may have missed
      // it, so TorFox starts after all. (Had the setup seen it too, its
      // TorFox only opens a window in this one.)
      if (platform::UpdateBeingInstalled()) {
        test_log::Event("update_being_installed", {{"start_requested", asked}});
        test_log::Close();
        return 0;
      }
    }
    platform::ClearStartAfterUpdate();
    browser_app = new BrowserApp(options);
    app = browser_app;
  } else if (type == "renderer") {
    app = new RendererApp();
  } else {
    app = new OtherApp();
  }

  const int exit_code = CefExecuteProcess(args, app, sandbox_info);
  if (exit_code >= 0) {
    return exit_code;  // A sub-process finished.
  }

#if defined(OS_WIN)
  // The bootstrap executable must provide sandbox information. Without it
  // Chromium would run every process unsandboxed: refuse to start.
  if (!sandbox_info) {
    test_log::Event("fatal", {{"message", "sandbox unavailable"}});
    if (!kTestHooks) {
      platform::ShowFatalError("TorFox",
                               "TorFox cannot start because the Chromium sandbox is unavailable.");
    }
    return 1;
  }
#endif
  platform::MarkRunning();

  std::error_code ec;
  if (options.user_data_dir.empty() || !options.user_data_dir.is_absolute()) {
    platform::ShowFatalError("TorFox", "TorFox could not determine its data folder.");
    return 1;
  }
  std::filesystem::create_directories(options.user_data_dir / "User Data", ec);
  if (ec) {
    platform::ShowFatalError("TorFox", "TorFox could not create its data folder.");
    return 1;
  }
  test_log::Event("startup", {{"sandbox_info", sandbox_info != nullptr},
                              {"user_data", platform::PathToUtf8(options.user_data_dir)}});
  // What's new: the version that last ran with this profile. Only an older
  // one means TorFox has been updated.
  {
    // The version TorFox last handed to an update's setup, if any: read once.
    const std::filesystem::path handoff_record = options.user_data_dir / kUpdateHandoffFile;
    const ReadResult h = ReadFileLimited(handoff_record, update::kMaxLastVersionBytes);
    const update::HandoffOutcome handoff = update::CheckHandoff(
        h.status == ReadStatus::kOk ? h.data : std::string(), kTorFoxVersion);
    if (h.status != ReadStatus::kMissing) {
      std::filesystem::remove(handoff_record, ec);
      ec.clear();
    }
    if (handoff == update::HandoffOutcome::kNotInstalled) {
      const std::string version = update::RecordedVersion(h.data).value_or("");
      browser_app->add_startup_notice("TorFox could not install the update to version " + version +
                                      ". This is still version " + kTorFoxVersion +
                                      ". You can try again from Settings > Updates.");
    }
    const std::filesystem::path record = options.user_data_dir / "last-version";
    const ReadResult r = ReadFileLimited(record, update::kMaxLastVersionBytes);
    const std::string text = r.status == ReadStatus::kOk ? r.data : std::string();
    const std::optional<std::string> previous = update::RecordedVersion(text);
    const bool updated = update::ShouldShowWhatsNew(text, kTorFoxVersion);
    test_log::Event("update_handoff_record",
                    {{"outcome", handoff == update::HandoffOutcome::kInstalled      ? "installed"
                                 : handoff == update::HandoffOutcome::kNotInstalled ? "not_installed"
                                                                                    : "none"}});
    if (updated) {
      browser_app->set_whats_new_from(*previous,
                                      handoff == update::HandoffOutcome::kInstalled);
    }
    if (previous != std::optional<std::string>(kTorFoxVersion)) {
      const bool ok = WriteFileAtomically(record, std::string(kTorFoxVersion) + "\n");
      test_log::Event("last_version", {{"previous", previous.value_or("")},
                                       {"updated", updated},
                                       {"saved", ok}});
    }
  }
  // Chromium's own record of downloads (docs/STAGE3_FINDINGS.md), erased
  // while Chromium is not running: now, and again after CefShutdown. The
  // normal profile's folder is the one ContextManager gives it.
  const std::filesystem::path normal_profile = options.user_data_dir / "User Data" / "Normal";
  {
    const chromium_profile::ScrubResult scrub = chromium_profile::ScrubDownloadRecords(normal_profile);
    test_log::Event("download_records_erased", {{"ok", scrub.ok},
                                                {"rows", scrub.rows},
                                                {"metadata", scrub.metadata_removed}});
  }
  const clear_data::Categories clear_at_start = ClearOnExitSetting(options.user_data_dir);
  // The normal profile's proxy mode, before Chromium creates its network
  // context (docs/STAGE1_FINDINGS.md §4, STAGE4_FINDINGS.md).
  {
    const char* mode = UseSystemProxySetting(options.user_data_dir) ? "system" : "direct";
    const bool ok = chromium_profile::WriteProxyMode(normal_profile, mode);
    test_log::Event("proxy_mode_written", {{"mode", mode}, {"ok", ok}});
    if (!ok) {
      browser_app->add_startup_notice(
          "TorFox could not save its network settings for this profile. If Windows looks for a "
          "proxy automatically, TorFox may have asked the local network for one as it started.");
    }
  }
  // Chromium's per-site scores, always; zoom levels with the history.
  {
    const chromium_profile::SiteRecordsResult sites =
        chromium_profile::ScrubSiteRecords(normal_profile, clear_at_start.history);
    test_log::Event("site_records_erased", {{"ok", sites.ok}, {"removed", sites.removed}});
  }

  // Clear when TorFox closes: done after Chromium shuts down, and again now,
  // before Chromium opens the profile, in case the last session ended
  // without it (a crash, or files that were in use).
  if (clear_at_start.any()) {
    const clear_data::Result r = clear_data::Clear(normal_profile, clear_at_start);
    test_log::Event("data_cleared", {{"when", "startup"},
                                     {"removed", r.removed},
                                     {"failed", r.failed}});
    if (r.failed > 0) {
      browser_app->add_startup_notice(
          "TorFox could not clear all of the browsing data you asked it to clear when it "
          "closes. Some of it is still on this computer; TorFox will try again next time.");
    }
  }

  CefSettings settings;
  settings.no_sandbox = false;
  settings.persist_session_cookies = false;
  settings.background_color = 0xFF1C1B22;
  // Release builds ignore Chromium/CEF switches from the command line
  // entirely; OnBeforeCommandLineProcessing applies TorFox's policy on top.
  settings.command_line_args_disabled = !kTestHooks;
  CefString(&settings.root_cache_path) =
      platform::PathToUtf8(options.user_data_dir / "User Data");
  // An empty cache_path keeps the global context in memory; it hosts no
  // content anyway.
  if (kTestHooks) {
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.log_file) = platform::PathToUtf8(options.user_data_dir / "cef.log");
  } else {
    // No logs: they could record URLs.
    settings.log_severity = LOGSEVERITY_DISABLE;
  }

  if (!CefInitialize(args, settings, app.get(), sandbox_info)) {
    const int code = CefGetExitCode();
    test_log::Event("cef_initialize_failed", {{"code", code}});
    test_log::Close();
    return code;
  }
  CefRunMessageLoop();
  browser_app->Shutdown();
  const clear_data::Categories clear_at_exit = browser_app->exit_clear();
  const std::optional<BrowserApp::UpdateHandoff> update = browser_app->update_handoff();
  if (update) {
    // A TorFox opened from now on waits for the setup, as one opened while
    // the setup installs does: this one is about to hand over.
    platform::HoldUpdateInstalling();
  }
  browser_app = nullptr;
  app = nullptr;
  CefShutdown();
  chromium_profile::ScrubDownloadRecords(normal_profile);
  chromium_profile::ScrubSiteRecords(normal_profile, clear_at_exit.history);
  // A failure here cannot be shown; the next start clears again and says so.
  clear_data::Clear(normal_profile, clear_at_exit);
  // "Relaunch to update", or TorFox closed with an update waiting: last,
  // once Chromium has let go of everything. The file is checked once more as
  // it is started.
  if (update) {
    // What the next start compares itself with (update::CheckHandoff). On
    // macOS the user copies the app from the disk image, so TorFox cannot
    // tell what was installed: no record.
    const std::filesystem::path handoff_record = options.user_data_dir / kUpdateHandoffFile;
    if (platform::InstallsUpdatesOnExit()) {
      WriteFileAtomically(handoff_record, update->version + "\n");
    }
    std::string error;
    const bool launched = platform::LaunchUpdate(update->file, update->sha256, update->size,
                                                 update->relaunch, &error);
    if (!launched) {
      platform::ReleaseUpdateInstalling();
      std::filesystem::remove(handoff_record, ec);
    }
    if (!launched && !kTestHooks) {
      platform::ShowFatalError("TorFox",
                               "TorFox could not install the update: " + error +
                                   (update->relaunch
                                        ? ".\n\nTorFox has not changed. You can try again from "
                                          "Settings > Updates."
                                        : ".\n\nTorFox has not changed. It will download the "
                                          "update again later."));
    }
  }
  return 0;
}

}  // namespace torfox
