// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Stage 0 feasibility spikes (see spike.h). Each scenario records events and
// values into a JSON results file and then shuts down cleanly.
//
//   --spike=a  sandbox/bootstrap smoke: window + one tab, held open for an
//              external sandbox inspection.
//   --spike=b  close individual tab BrowserViews without closing the window.
//   --spike=c  per-context SOCKS proxy, fail-closed, OTR cookie isolation,
//              .onion host-resolver rule.
//   --spike=d  Alloy-style pref readback, WebRTC ICE candidates, HTTPS-First.

#include "spikes/cefspike/spike.h"

#include <fstream>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <deque>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_cookie.h"
#include "include/cef_parser.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_task.h"
#include "include/cef_version.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

namespace {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

class FnTask : public CefTask {
 public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override { fn_(); }

 private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(FnTask);
};

void PostUI(std::function<void()> fn, int64_t delay_ms = 0) {
  CefPostDelayedTask(TID_UI, new FnTask(std::move(fn)), delay_ms);
}

std::string ToJson(CefRefPtr<CefValue> value) {
  if (!value) {
    return "null";
  }
  return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

CefRefPtr<CefValue> StringValue(const std::string& s) {
  auto v = CefValue::Create();
  v->SetString(s);
  return v;
}

CefRefPtr<CefValue> IntValue(int i) {
  auto v = CefValue::Create();
  v->SetInt(i);
  return v;
}

CefRefPtr<CefValue> BoolValue(bool b) {
  auto v = CefValue::Create();
  v->SetBool(b);
  return v;
}

std::string JsonEscape(const std::string& in) {
  std::string out = "\"";
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

// Holds only plain C++ data so nothing CEF-owned outlives CefShutdown().
class Results {
 public:
  static Results& Get() {
    static Results r;
    return r;
  }
  void Set(const std::string& key, CefRefPtr<CefValue> value) {
    values_[key] = ToJson(value);
  }
  // Events are flushed immediately so a crash still leaves evidence behind.
  void Event(const std::string& e) {
    events_.push_back(e);
    if (!path_.empty()) {
      Write(path_);
    }
  }
  void SetPath(const std::string& path) { path_ = path; }
  void Write(const std::string& path) {
    std::string json = "{\n";
    for (const auto& [k, v] : values_) {
      json += "  " + JsonEscape(k) + ": " + v + ",\n";
    }
    json += "  \"events\": [\n";
    for (size_t i = 0; i < events_.size(); ++i) {
      json += "    " + JsonEscape(events_[i]) + (i + 1 < events_.size() ? ",\n" : "\n");
    }
    json += "  ]\n}\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
  }

 private:
  std::map<std::string, std::string> values_;
  std::vector<std::string> events_;
  std::string path_;
};

std::string Switch(const char* name) {
  return CefCommandLine::GetGlobalCommandLine()->GetSwitchValue(name).ToString();
}

// ---------------------------------------------------------------------------
// Client / delegates
// ---------------------------------------------------------------------------

class SpikeClient : public CefClient,
                    public CefLifeSpanHandler,
                    public CefLoadHandler,
                    public CefDisplayHandler,
                    public CefRequestHandler {
 public:
  explicit SpikeClient(std::string tag) : tag_(std::move(tag)) {}

  std::function<void(CefRefPtr<CefBrowser>)> on_after_created;
  std::function<void(CefRefPtr<CefBrowser>, int)> on_load_end;
  std::function<void(CefRefPtr<CefBrowser>, int, const std::string&)> on_load_error;
  std::function<void(const std::string&)> on_console;
  std::function<bool(CefRefPtr<CefBrowser>)> on_do_close;
  std::function<void(CefRefPtr<CefBrowser>)> on_before_close;

  const std::string& tag() const { return tag_; }

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Results::Get().Event(tag_ + ":after_created");
    if (on_after_created) {
      on_after_created(browser);
    }
  }
  bool DoClose(CefRefPtr<CefBrowser> browser) override {
    bool handled = on_do_close ? on_do_close(browser) : false;
    Results::Get().Event(tag_ + ":do_close returned " + (handled ? "true" : "false"));
    return handled;
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    Results::Get().Event(tag_ + ":before_close");
    if (on_before_close) {
      on_before_close(browser);
    }
  }
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int http_status) override {
    if (!frame->IsMain()) {
      return;
    }
    Results::Get().Event(tag_ + ":load_end " + std::to_string(http_status) + " " +
                         frame->GetURL().ToString());
    if (on_load_end) {
      on_load_end(browser, http_status);
    }
  }
  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode code,
                   const CefString& text,
                   const CefString& url) override {
    if (!frame->IsMain()) {
      return;
    }
    Results::Get().Event(tag_ + ":load_error " + std::to_string(code) + " " +
                         text.ToString() + " " + url.ToString());
    if (on_load_error) {
      on_load_error(browser, code, url.ToString());
    }
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override {
    if (on_console) {
      on_console(message.ToString());
    }
    return false;
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override {
    if (frame->IsMain()) {
      Results::Get().Event(tag_ + ":before_browse " + request->GetURL().ToString() +
                           (is_redirect ? " (redirect)" : ""));
    }
    return false;
  }

 private:
  std::string tag_;
  IMPLEMENT_REFCOUNTING(SpikeClient);
};

class SpikeBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit SpikeBrowserViewDelegate(int preferred_height = 0)
      : preferred_height_(preferred_height) {}
  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return CefSize(0, preferred_height_);
  }

 private:
  int preferred_height_;
  IMPLEMENT_REFCOUNTING(SpikeBrowserViewDelegate);
};

class SpikeWindowDelegate : public CefWindowDelegate {
 public:
  std::function<void(CefRefPtr<CefWindow>)> on_created;
  std::function<bool(CefRefPtr<CefWindow>)> can_close;
  std::function<void()> on_destroyed;

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    Results::Get().Event("window:created");
    if (on_created) {
      on_created(window);
    }
    window->Show();
  }
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    Results::Get().Event("window:destroyed");
    if (on_destroyed) {
      on_destroyed();
    }
  }
  bool CanClose(CefRefPtr<CefWindow> window) override {
    bool ok = can_close ? can_close(window) : true;
    Results::Get().Event(std::string("window:can_close ") + (ok ? "true" : "false"));
    return ok;
  }
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return CefSize(1000, 700);
  }
  cef_runtime_style_t GetWindowRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

 private:
  IMPLEMENT_REFCOUNTING(SpikeWindowDelegate);
};

class CtxHandler : public CefRequestContextHandler {
 public:
  explicit CtxHandler(std::function<void(CefRefPtr<CefRequestContext>)> fn)
      : fn_(std::move(fn)) {}
  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> ctx) override {
    fn_(ctx);
  }

 private:
  std::function<void(CefRefPtr<CefRequestContext>)> fn_;
  IMPLEMENT_REFCOUNTING(CtxHandler);
};

class SetCookieCb : public CefSetCookieCallback {
 public:
  explicit SetCookieCb(std::string tag) : tag_(std::move(tag)) {}
  void OnComplete(bool success) override {
    Results::Get().Event("cookie_set:" + tag_ + (success ? " ok" : " FAILED"));
  }

 private:
  std::string tag_;
  IMPLEMENT_REFCOUNTING(SetCookieCb);
};

class CookieCollector : public CefCookieVisitor {
 public:
  bool Visit(const CefCookie& cookie, int, int, bool&) override {
    names.push_back(CefString(&cookie.name).ToString());
    return true;
  }
  std::vector<std::string> names;

 private:
  IMPLEMENT_REFCOUNTING(CookieCollector);
};

// ---------------------------------------------------------------------------
// Spike driver
// ---------------------------------------------------------------------------

struct Tab {
  CefRefPtr<SpikeClient> client;
  CefRefPtr<CefBrowserView> view;
  bool closing = false;
  bool closed = false;
};

class Spike {
 public:
  void Start() {
    mode_ = Switch("spike");
    out_path_ = Switch("spike-out");
    Results::Get().SetPath(out_path_);
    http_port_ = Switch("http-port");
    socks_port_ = Switch("socks-port");
    dead_port_ = Switch("dead-port");
    Results::Get().Set("mode", StringValue(mode_));
    Results::Get().Set("cef_version", StringValue(CEF_VERSION));
    Results::Get().Set("chrome_version",
                       StringValue(std::to_string(CHROME_VERSION_MAJOR) + "." +
                                   std::to_string(CHROME_VERSION_MINOR) + "." +
                                   std::to_string(CHROME_VERSION_BUILD) + "." +
                                   std::to_string(CHROME_VERSION_PATCH)));
    Results::Get().Set(
        "browser_command_line",
        StringValue(CefCommandLine::GetGlobalCommandLine()->GetCommandLineString().ToString()));

    // Global watchdog: never hang the harness.
    PostUI([this] {
      if (!finished_) {
        Results::Get().Event("TIMEOUT");
        Results::Get().Set("timeout", BoolValue(true));
        Finish();
        PostUI([] { CefQuitMessageLoop(); }, 5000);
      }
    }, 60000);

    if (mode_ == "a") {
      StartA();
    } else if (mode_ == "b") {
      StartB();
    } else if (mode_ == "c") {
      StartC();
    } else if (mode_ == "d") {
      StartD();
    } else {
      Results::Get().Event("unknown spike mode");
      Finish();
    }
  }

  void SetSandboxed(bool sandboxed) {
    Results::Get().Set("sandbox_info_present", BoolValue(sandboxed));
  }

 private:
  // ----- window / tabs -------------------------------------------------------

  void CreateWindowWithTabs(std::function<void()> after_created) {
    auto delegate = new SpikeWindowDelegate();
    delegate->on_created = [this, after_created](CefRefPtr<CefWindow> window) {
      window_ = window;
      CefBoxLayoutSettings box;
      box.horizontal = false;
      auto layout = window->SetToBoxLayout(box);

      // "Toolbar" stand-in: an Alloy BrowserView with a fixed height.
      toolbar_client_ = new SpikeClient("toolbar");
      AttachCloseTracking(toolbar_client_);
      toolbar_ = CefBrowserView::CreateBrowserView(
          toolbar_client_, "data:text/html,<body style='background:%23222;color:%23eee'>toolbar</body>",
          CefBrowserSettings(), nullptr, nullptr, new SpikeBrowserViewDelegate(48));
      window->AddChildView(toolbar_);
      layout->SetFlexForView(toolbar_, 0);

      content_ = CefPanel::CreatePanel(nullptr);
      content_->SetToFillLayout();
      window->AddChildView(content_);
      layout->SetFlexForView(content_, 1);
      after_created();
    };
    delegate->can_close = [this](CefRefPtr<CefWindow>) {
      closing_all_ = true;
      bool all = true;
      for (auto& view : AllLiveViews()) {
        auto browser = view->GetBrowser();
        if (browser && !browser->GetHost()->TryCloseBrowser()) {
          all = false;
        }
      }
      return all;
    };
    delegate->on_destroyed = [this] {
      window_ = nullptr;
      content_ = nullptr;
      toolbar_ = nullptr;
      MaybeQuit();
    };
    CefWindow::CreateTopLevelWindow(delegate);
  }

  void AttachCloseTracking(CefRefPtr<SpikeClient> client) {
    client->on_after_created = [this](CefRefPtr<CefBrowser>) { live_browsers_++; };
    client->on_before_close = [this](CefRefPtr<CefBrowser>) {
      live_browsers_--;
      MaybeQuit();
    };
  }

  Tab& AddTab(const std::string& tag,
              const std::string& url,
              CefRefPtr<CefRequestContext> ctx,
              bool visible) {
    Tab tab;
    tab.client = new SpikeClient(tag);
    AttachCloseTracking(tab.client);
    tab.view = CefBrowserView::CreateBrowserView(tab.client, url, CefBrowserSettings(),
                                                 nullptr, ctx, new SpikeBrowserViewDelegate());
    content_->AddChildView(tab.view);
    tab.view->SetVisible(visible);
    tabs_.push_back(tab);
    return tabs_.back();
  }

  std::vector<CefRefPtr<CefBrowserView>> AllLiveViews() {
    std::vector<CefRefPtr<CefBrowserView>> v;
    if (toolbar_) {
      v.push_back(toolbar_);
    }
    for (auto& t : tabs_) {
      if (!t.closed && t.view) {
        v.push_back(t.view);
      }
    }
    return v;
  }

  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    Results::Get().Event("finish");
    Results::Get().Write(out_path_);
    if (window_) {
      window_->Close();
    } else {
      MaybeQuit();
    }
  }

  void MaybeQuit() {
    if (finished_ && live_browsers_ <= 0 && !window_) {
      Results::Get().Event("quit");
      Results::Get().Write(out_path_);
      CefQuitMessageLoop();
    }
  }

  // ----- Spike A: sandbox/bootstrap smoke -----------------------------------

  void StartA() {
    CreateWindowWithTabs([this] {
      auto& tab = AddTab("tab", "http://127.0.0.1:" + http_port_ + "/hello", nullptr, true);
      tab.client->on_load_end = [this](CefRefPtr<CefBrowser>, int status) {
        Results::Get().Set("loaded_status", IntValue(status));
        int hold = std::atoi(Switch("spike-hold").c_str());
        PostUI([this] { Finish(); }, (hold > 0 ? hold : 3) * 1000);
      };
    });
  }

  // ----- Spike B: close a single tab ----------------------------------------

  void StartB() {
    CreateWindowWithTabs([this] {
      for (int i = 1; i <= 3; ++i) {
        std::string tag = "tab" + std::to_string(i);
        auto& tab = AddTab(tag, "data:text/html,<h1>" + tag + "</h1>", nullptr, i == 1);
        tab.client->on_load_end = [this](CefRefPtr<CefBrowser>, int) { OnBLoaded(); };
        tab.client->on_do_close = [this, i](CefRefPtr<CefBrowser>) {
          Tab& t = tabs_[i - 1];
          if (closing_all_ || !t.closing) {
            return false;  // window teardown: default behaviour
          }
          // Single-tab close: detach the view and drop every reference so the
          // view hierarchy tear-down completes the browser close.
          PostUI([this, i] {
            Tab& tab = tabs_[i - 1];
            if (content_ && tab.view) {
              content_->RemoveChildView(tab.view);
              tab.view = nullptr;
              Results::Get().Event("view_removed_and_released");
            }
          });
          return true;
        };
      }
    });
  }

  void OnBLoaded() {
    if (++b_loaded_ != 3) {
      return;
    }
    Results::Get().Event("all_tabs_loaded");
    // Activate tab2 then close it (closing the *visible* tab).
    tabs_[0].view->SetVisible(false);
    tabs_[1].view->SetVisible(true);
    tabs_[1].closing = true;
    tabs_[1].client->on_before_close = [this](CefRefPtr<CefBrowser>) {
      live_browsers_--;
      tabs_[1].closed = true;
      tabs_[1].view = nullptr;
      PostUI([this] { BAfterFirstClose(); }, 300);
    };
    tabs_[1].view->GetBrowser()->GetHost()->CloseBrowser(false);
  }

  void BAfterFirstClose() {
    Results::Get().Set("b_window_valid_after_close_visible",
                       BoolValue(window_ && window_->IsValid() && !window_->IsClosed()));
    // Close tab1, which is hidden, the same way.
    tabs_[0].closing = true;
    tabs_[0].client->on_before_close = [this](CefRefPtr<CefBrowser>) {
      live_browsers_--;
      tabs_[0].closed = true;
      tabs_[0].view = nullptr;
      PostUI([this] { BAfterSecondClose(); }, 300);
    };
    tabs_[0].view->GetBrowser()->GetHost()->CloseBrowser(false);
  }

  void BAfterSecondClose() {
    Results::Get().Set("b_window_valid_after_close_hidden",
                       BoolValue(window_ && window_->IsValid() && !window_->IsClosed()));
    // Remaining tab must still work.
    tabs_[2].view->SetVisible(true);
    tabs_[2].client->on_load_end = [this](CefRefPtr<CefBrowser>, int) {
      Results::Get().Set("b_remaining_tab_navigated", BoolValue(true));
      PostUI([this] { Finish(); }, 300);
    };
    tabs_[2].view->GetBrowser()->GetMainFrame()->LoadURL(
        "data:text/html,<h1>tab3 after closes</h1>");
  }

  // ----- Spike C: per-context proxy + isolation ------------------------------

  CefRefPtr<CefDictionaryValue> ProxyDict(const std::string& port) {
    auto d = CefDictionaryValue::Create();
    d->SetString("mode", "fixed_servers");
    d->SetString("server", "socks5://127.0.0.1:" + port);
    d->SetString("bypass_list", "<-loopback>");
    return d;
  }

  bool ApplyProxy(CefRefPtr<CefRequestContext> ctx, const std::string& port,
                  const std::string& label) {
    auto v = CefValue::Create();
    v->SetDictionary(ProxyDict(port));
    CefString error;
    bool can = ctx->CanSetPreference("proxy");
    bool ok = ctx->SetPreference("proxy", v, error);
    Results::Get().Set(label + "_can_set", BoolValue(can));
    Results::Get().Set(label + "_set_ok", BoolValue(ok));
    Results::Get().Set(label + "_set_error", StringValue(error.ToString()));
    Results::Get().Set(label + "_readback", StringValue(ToJson(ctx->GetPreference("proxy"))));
    return ok;
  }

  void CreateContexts(std::function<void()> all_ready) {
    std::string root = Switch("spike-dir") + "\\profile_root";
    auto ready = std::make_shared<int>(0);
    auto check = [ready, all_ready] {
      if (++*ready == 3) {
        all_ready();
      }
    };

    CefRequestContextSettings normal_settings;
    CefString(&normal_settings.cache_path) = root + "\\normal";
    normal_ = CefRequestContext::CreateContext(
        normal_settings, new CtxHandler([this, check](CefRefPtr<CefRequestContext> ctx) {
          Results::Get().Event("ctx:normal initialized");
          check();
        }));

    // --ctx-variant: default (tor+private both OTR), disk-tor (tor on disk),
    // disk-private (private on disk), disk-both.
    const std::string variant = Switch("ctx-variant");
    Results::Get().Set("ctx_variant", StringValue(variant.empty() ? "default" : variant));
    CefRequestContextSettings tor_settings;  // empty cache_path => OTR
    if (variant == "disk-tor" || variant == "disk-both") {
      CefString(&tor_settings.cache_path) = root + "\\tor";
    }
    CefRequestContextSettings private_settings;
    if (variant == "disk-private" || variant == "disk-both") {
      CefString(&private_settings.cache_path) = root + "\\private";
    }
    tor_ = CefRequestContext::CreateContext(
        tor_settings, new CtxHandler([this, check](CefRefPtr<CefRequestContext> ctx) {
          Results::Get().Event("ctx:tor initialized");
          if (Switch("no-proxy") != "1") {
            ApplyProxy(ctx, socks_port_, "tor_proxy_initial");
          }
          check();
        }));
    private_ = CefRequestContext::CreateContext(
        private_settings, new CtxHandler([this, check](CefRefPtr<CefRequestContext> ctx) {
          Results::Get().Event("ctx:private initialized");
          check();
        }));
  }

  void StartC() {
    CreateContexts([this] {
      auto& R = Results::Get();
      R.Set("private_proxy_readback", StringValue(ToJson(private_->GetPreference("proxy"))));
      R.Set("normal_proxy_readback", StringValue(ToJson(normal_->GetPreference("proxy"))));
      R.Set("global_proxy_readback",
            StringValue(ToJson(CefRequestContext::GetGlobalContext()->GetPreference("proxy"))));
      R.Set("tor_is_same_as_private", BoolValue(tor_->IsSame(private_)));
      R.Set("tor_sharing_with_private", BoolValue(tor_->IsSharingWith(private_)));
      R.Set("tor_sharing_with_global",
            BoolValue(tor_->IsSharingWith(CefRequestContext::GetGlobalContext())));
      CreateWindowWithTabs([this] { CStep1(); });
    });
  }

  // Step 1: Tor-context tab loads through the fake SOCKS proxy.
  void CStep1() {
    auto& tor_tab = AddTab("tor", "http://spike-direct.test:" + http_port_ + "/via-tor", tor_, true);
    tor_tab.client->on_load_end = [this](CefRefPtr<CefBrowser> b, int status) {
      if (c_step_ == 0) {
        c_step_ = 1;
        Results::Get().Set("c_tor_load_status", IntValue(status));
        // Step 2: onion through Tor context.
        b->GetMainFrame()->LoadURL(
            "http://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion:" +
            http_port_ + "/onion-via-tor");
      } else if (c_step_ == 1) {
        c_step_ = 2;
        Results::Get().Set("c_tor_onion_status", IntValue(status));
        CStep3();
      }
    };
    tor_tab.client->on_load_error = [this](CefRefPtr<CefBrowser>, int code, const std::string&) {
      if (c_step_ == 0 || c_step_ == 1) {
        Results::Get().Set("c_tor_step" + std::to_string(c_step_) + "_error", IntValue(code));
        c_step_ = 2;
        CStep3();
      } else if (c_step_ == 4) {
        Results::Get().Set("c_dead_proxy_error", IntValue(code));
        c_step_ = 5;
        tabs_[0].view->GetBrowser()->GetMainFrame()->LoadURL(
            "http://127.0.0.1:" + http_port_ + "/after-dead-loopback");
      } else if (c_step_ == 5) {
        Results::Get().Set("c_dead_proxy_loopback_error", IntValue(code));
        c_step_ = 6;
        CStep7Cookies();
      }
    };
  }

  // Step 3: Normal-context tab tries the same onion; must not resolve.
  void CStep3() {
    tabs_[0].view->SetVisible(false);
    auto& normal_tab = AddTab(
        "normal",
        "http://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion:" + http_port_ +
            "/onion-via-normal",
        normal_, true);
    normal_tab.client->on_load_error = [this](CefRefPtr<CefBrowser>, int code, const std::string&) {
      Results::Get().Set("c_normal_onion_error", IntValue(code));
      CStep4DeadProxy();
    };
    normal_tab.client->on_load_end = [this](CefRefPtr<CefBrowser>, int status) {
      if (status != 0) {
        Results::Get().Set("c_normal_onion_UNEXPECTED_load", IntValue(status));
        CStep4DeadProxy();
      }
    };
  }

  // Step 4: point the Tor context at a dead SOCKS port; requests must fail
  // without any direct connection.
  void CStep4DeadProxy() {
    if (c_step_ >= 4) {
      return;
    }
    c_step_ = 4;
    ApplyProxy(tor_, dead_port_, "tor_proxy_dead");
    tabs_[0].view->GetBrowser()->GetMainFrame()->LoadURL(
        "http://spike-direct.test:" + http_port_ + "/after-dead");
  }

  // Step 7: cookie isolation between contexts.
  void CStep7Cookies() {
    auto make_cookie = [](const std::string& name) {
      CefCookie c;
      CefString(&c.name) = name;
      CefString(&c.value) = "1";
      CefString(&c.path) = "/";
      return c;
    };
    const std::string url = "http://spike-cookie.test/";
    tor_->GetCookieManager(nullptr)->SetCookie(url, make_cookie("tor_cookie"), new SetCookieCb("tor"));
    private_->GetCookieManager(nullptr)->SetCookie(url, make_cookie("private_cookie"),
                                                   new SetCookieCb("private"));
    normal_->GetCookieManager(nullptr)->SetCookie(url, make_cookie("normal_cookie"),
                                                  new SetCookieCb("normal"));
    PostUI([this, url] {
      tor_cookies_ = new CookieCollector();
      private_cookies_ = new CookieCollector();
      normal_cookies_ = new CookieCollector();
      tor_->GetCookieManager(nullptr)->VisitUrlCookies(url, true, tor_cookies_);
      private_->GetCookieManager(nullptr)->VisitUrlCookies(url, true, private_cookies_);
      normal_->GetCookieManager(nullptr)->VisitUrlCookies(url, true, normal_cookies_);
      PostUI([this] {
        auto join = [](const std::vector<std::string>& v) {
          std::string s;
          for (auto& n : v) {
            s += (s.empty() ? "" : ",") + n;
          }
          return s;
        };
        Results::Get().Set("c_cookies_tor", StringValue(join(tor_cookies_->names)));
        Results::Get().Set("c_cookies_private", StringValue(join(private_cookies_->names)));
        Results::Get().Set("c_cookies_normal", StringValue(join(normal_cookies_->names)));
        Finish();
      }, 1500);
    }, 1000);
  }

  // ----- Spike D: prefs + WebRTC + HTTPS-First ------------------------------

  void SetPref(CefRefPtr<CefRequestContext> ctx, const std::string& label,
               const std::string& name, CefRefPtr<CefValue> value) {
    CefString error;
    bool can = ctx->CanSetPreference(name);
    bool ok = ctx->SetPreference(name, value, error);
    Results::Get().Set("d_" + label + "_" + name,
                       StringValue(std::string("can=") + (can ? "1" : "0") + " ok=" + (ok ? "1" : "0") +
                                   " err='" + error.ToString() + "' readback=" +
                                   ToJson(ctx->GetPreference(name))));
  }

  void StartD() {
    CreateContexts([this] {
      // Normal context: privacy prefs.
      SetPref(normal_, "normal", "net.network_prediction_options", IntValue(2));
      SetPref(normal_, "normal", "profile.cookie_controls_mode", IntValue(1));
      SetPref(normal_, "normal", "https_only_mode_enabled", BoolValue(true));
      SetPref(normal_, "normal", "webrtc.ip_handling_policy",
              StringValue("default_public_interface_only"));
      // Tor context: WebRTC lock-down + prediction off.
      SetPref(tor_, "tor", "webrtc.ip_handling_policy", StringValue("disable_non_proxied_udp"));
      SetPref(tor_, "tor", "net.network_prediction_options", IntValue(2));
      // Private context: check whether cookie_controls_mode written on normal
      // or tor bleeds in (allowlisted prefs write through to the parent).
      Results::Get().Set("d_private_cookie_controls_mode_readback",
                         StringValue(ToJson(private_->GetPreference("profile.cookie_controls_mode"))));
      Results::Get().Set("d_private_webrtc_readback",
                         StringValue(ToJson(private_->GetPreference("webrtc.ip_handling_policy"))));
      Results::Get().Set("d_global_cookie_controls_mode_readback",
                         StringValue(ToJson(CefRequestContext::GetGlobalContext()->GetPreference(
                             "profile.cookie_controls_mode"))));
      CreateWindowWithTabs([this] { DWebRTC(); });
    });
  }

  void DWebRTC() {
    auto& p = AddTab("private_webrtc", "http://127.0.0.1:" + http_port_ + "/webrtc", private_, false);
    p.client->on_console = [this](const std::string& m) { DIce("private", m); };
    auto& n = AddTab("normal_webrtc", "http://127.0.0.1:" + http_port_ + "/webrtc", normal_, false);
    n.client->on_console = [this](const std::string& m) { DIce("normal", m); };
    auto& t = AddTab("tor_webrtc", "http://spike-direct.test:" + http_port_ + "/webrtc", tor_, true);
    t.client->on_console = [this](const std::string& m) { DIce("tor", m); };
  }

  void DIce(const std::string& who, const std::string& msg) {
    if (msg.rfind("ICE_", 0) != 0) {
      return;
    }
    Results::Get().Set("d_ice_" + who, StringValue(msg));
    if (++d_ice_done_ == 3) {
      DHttpsFirst();
    }
  }

  void DHttpsFirst() {
    // Live network check: does Chromium's HTTPS-First upgrade apply to an
    // Alloy-style browser when https_only_mode_enabled is set?
    https_url_ = Switch("https-test-url");
    if (https_url_.empty()) {
      https_url_ = "http://example.com/";
    }
    auto& n = AddTab("normal_https_first", https_url_, normal_, true);
    n.client->on_load_end = [this](CefRefPtr<CefBrowser> b, int status) {
      Results::Get().Set("d_https_first_final_url", StringValue(b->GetMainFrame()->GetURL().ToString()));
      Results::Get().Set("d_https_first_status", IntValue(status));
      DHttpsControl();
    };
    n.client->on_load_error = [this](CefRefPtr<CefBrowser>, int code, const std::string& url) {
      Results::Get().Set("d_https_first_error", StringValue(std::to_string(code) + " " + url));
      DHttpsControl();
    };
  }

  void DHttpsControl() {
    if (d_control_started_) {
      return;
    }
    d_control_started_ = true;
    auto& p = AddTab("private_http_control", https_url_, private_, false);
    p.client->on_load_end = [this](CefRefPtr<CefBrowser> b, int status) {
      Results::Get().Set("d_http_control_final_url", StringValue(b->GetMainFrame()->GetURL().ToString()));
      PostUI([this] { Finish(); }, 300);
    };
    p.client->on_load_error = [this](CefRefPtr<CefBrowser>, int code, const std::string& url) {
      Results::Get().Set("d_http_control_error", StringValue(std::to_string(code) + " " + url));
      PostUI([this] { Finish(); }, 300);
    };
  }

  std::string mode_, out_path_, http_port_, socks_port_, dead_port_, https_url_;
  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefPanel> content_;
  CefRefPtr<CefBrowserView> toolbar_;
  CefRefPtr<SpikeClient> toolbar_client_;
  std::deque<Tab> tabs_;
  int live_browsers_ = 0;
  bool finished_ = false;
  bool closing_all_ = false;
  int b_loaded_ = 0;
  int c_step_ = 0;
  int d_ice_done_ = 0;
  bool d_control_started_ = false;
  CefRefPtr<CefRequestContext> normal_, tor_, private_;
  CefRefPtr<CookieCollector> tor_cookies_, private_cookies_, normal_cookies_;
};

// ---------------------------------------------------------------------------
// CefApp
// ---------------------------------------------------------------------------

class SpikeApp : public CefApp, public CefBrowserProcessHandler {
 public:
  explicit SpikeApp(bool sandboxed) : sandboxed_(sandboxed) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override {
    if (process_type.empty()) {
      // Local DNS must never resolve .onion; SOCKS5 resolution is proxy-side.
      command_line->AppendSwitchWithValue("host-resolver-rules", "MAP *.onion ~NOTFOUND");
      // Chromium keeps only one value per switch, so extra disabled features
      // must be merged into the value CEF already set.
      const std::string extra = command_line->GetSwitchValue("spike-disable-features").ToString();
      if (!extra.empty()) {
        std::string merged = command_line->GetSwitchValue("disable-features").ToString();
        merged += (merged.empty() ? "" : ",") + extra;
        command_line->AppendSwitchWithValue("disable-features", merged);
      }
    }
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();
    spike_ = std::make_unique<Spike>();
    spike_->SetSandboxed(sandboxed_);
    spike_->Start();
  }

  // All CEF object references must be released before CefShutdown().
  void ReleaseSpike() { spike_.reset(); }

 private:
  bool sandboxed_;
  std::unique_ptr<Spike> spike_;
  IMPLEMENT_REFCOUNTING(SpikeApp);
};

}  // namespace

int SpikeMain(const CefMainArgs& main_args, void* sandbox_info) {
  CefRefPtr<SpikeApp> app(new SpikeApp(sandbox_info != nullptr));
  int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
  if (exit_code >= 0) {
    return exit_code;
  }

  std::string dir;
  {
    CefRefPtr<CefCommandLine> cl = CefCommandLine::CreateCommandLine();
#if defined(OS_WIN)
    cl->InitFromString(::GetCommandLineW());
#endif
    dir = cl->GetSwitchValue("spike-dir").ToString();
  }
  if (dir.empty()) {
    return 2;
  }

  CefSettings settings;
  settings.no_sandbox = sandbox_info == nullptr;
  CefString(&settings.root_cache_path) = dir + "\\profile_root";
  CefString(&settings.cache_path) = dir + "\\profile_root\\base";
  CefString(&settings.log_file) = dir + "\\cef.log";
  settings.log_severity = LOGSEVERITY_WARNING;
  settings.persist_session_cookies = false;

  if (!CefInitialize(main_args, settings, app.get(), sandbox_info)) {
    return CefGetExitCode();
  }
  CefRunMessageLoop();
  app->ReleaseSpike();
  CefShutdown();
  return 0;
}
