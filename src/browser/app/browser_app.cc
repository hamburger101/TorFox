// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/browser_app.h"

#include <system_error>
#include <vector>

#include "include/cef_request_context.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/app_common.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/renderer_app.h"
#include "src/browser/app/services.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/fault_injection.h"
#include "src/browser/app/test_log.h"
#include "src/browser/bridge/bridge_handler.h"
#include "src/browser/platform/platform.h"
#include "src/browser/scheme/internal_scheme.h"
#include "src/browser/shell/browser_window.h"
#include "src/core/cmdline/policy.h"

namespace torfox {

namespace {

Services g_services;

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

}  // namespace

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
  g_services.window_opened = [this](BrowserWindow* w) { windows_.insert(w); };
  g_services.window_destroyed = [this](BrowserWindow* w) {
    windows_.erase(w);
    if (windows_.empty()) {
      CefQuitMessageLoop();
    }
  };

  router_ = CefMessageRouterBrowserSide::Create(BridgeRouterConfig());
  bridge_ = std::make_unique<BridgeHandler>();
  router_->AddHandler(bridge_.get(), /*first=*/false);
  g_services.router = router_;

  contexts_ = std::make_unique<ContextManager>();
  g_services.contexts = contexts_.get();
  CefRefPtr<BrowserApp> self = this;
  contexts_->Initialize(options_.user_data_dir / "User Data", CreateInternalSchemeFactory(),
                        [self](bool ok, const std::string& err) {
                          if (!ok) {
                            self->Fatal(err);
                            return;
                          }
                          BrowserWindow::Open(self->options_.start_url);
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
  if (contexts_) {
    contexts_->Shutdown();
  }
  contexts_.reset();
  g_services = Services();
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
      const std::string start = raw->GetSwitchValue("torfox-test-start-url").ToString();
      if (cmdline::IsAbsoluteHttpUrl(start)) {
        options.start_url = start;
      }
    }
    if (options.start_url.empty()) {
      for (const std::string& arg : ArgsOf(raw)) {
        if (cmdline::IsAbsoluteHttpUrl(arg)) {
          options.start_url = arg;
          break;
        }
      }
    }
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
  browser_app = nullptr;
  app = nullptr;
  CefShutdown();
  return 0;
}

}  // namespace torfox
