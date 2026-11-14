// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/browser_window.h"

#include <algorithm>

#include "include/views/cef_box_layout.h"
#include "include/views/cef_fill_layout.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/app_common.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/services.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"
#include "src/browser/shell/tab_client.h"
#include "src/core/url/internal_url.h"
#include "src/core/url/nav_input.h"

namespace torfox {

namespace {

constexpr int kShellHeight = 82;  // DIP: tab strip + toolbar.
constexpr cef_color_t kDarkBackground = 0xFF1C1B22;
constexpr cef_color_t kLightBackground = 0xFFF6F5F9;

class ViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit ViewDelegate(BrowserRole role) : role_(role) {}

  // The shell has a fixed height; the BoxLayout stretches it horizontally.
  // CEF treats a size with any zero dimension as "use the default", so the
  // width must be non-zero. Tab views keep the defaults and are sized by the
  // content panel's FillLayout.
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return role_ == BrowserRole::kShell ? CefSize(1, kShellHeight) : CefSize();
  }
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
    return role_ == BrowserRole::kShell ? CefSize(1, kShellHeight) : CefSize();
  }
  int GetHeightForWidth(CefRefPtr<CefView> view, int width) override {
    return role_ == BrowserRole::kShell ? kShellHeight : 0;
  }
  cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

 private:
  const BrowserRole role_;
  IMPLEMENT_REFCOUNTING(ViewDelegate);
};

}  // namespace

// ----- Creation --------------------------------------------------------------

void BrowserWindow::Open(const std::string& first_url) {
  CEF_REQUIRE_UI_THREAD();
  CefWindow::CreateTopLevelWindow(new BrowserWindow(first_url));
}

BrowserWindow::BrowserWindow(std::string first_url) : first_url_(std::move(first_url)) {}

CefRefPtr<CefBrowserView> BrowserWindow::CreateView(BrowserRole role,
                                                    int tab_id,
                                                    const std::string& url,
                                                    CefRefPtr<TabClient>* client_out) {
  Services& services = GetServices();
  const bool internal = IsInternalRole(role);
  CefRefPtr<TabClient> client = new TabClient(role, tab_id, this, services.router, url);
  ++live_clients_;

  CefRefPtr<CefDictionaryValue> extra;
  if (internal) {
    extra = CefDictionaryValue::Create();
    extra->SetBool(kExtraInfoInternal, true);
  }
  CefBrowserSettings settings;
  settings.background_color =
      services.settings && services.settings->EffectiveDark() ? kDarkBackground : kLightBackground;
  if (internal) {
    // Internal pages need none of these.
    settings.javascript_close_windows = STATE_DISABLED;
    settings.javascript_access_clipboard = STATE_DISABLED;
    settings.javascript_dom_paste = STATE_DISABLED;
    settings.local_storage = STATE_DISABLED;
    settings.webgl = STATE_DISABLED;
    settings.remote_fonts = STATE_DISABLED;
  }
  CefRefPtr<CefRequestContext> context = services.contexts->Get(
      internal ? ContextManager::Kind::kInternal : ContextManager::Kind::kNormal);
  // Created without a URL: TabClient loads |url| once the browser is in the
  // registry, so the very first request is already subject to policy.
  CefRefPtr<CefBrowserView> view = CefBrowserView::CreateBrowserView(
      client, "", settings, extra, context, new ViewDelegate(role));
  *client_out = client;
  return view;
}

void BrowserWindow::OnWindowCreated(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  window_ = window;
  Services& services = GetServices();
  if (services.window_opened) {
    services.window_opened(this);
  }

  CefBoxLayoutSettings box;
  box.horizontal = false;
  CefRefPtr<CefBoxLayout> layout = window->SetToBoxLayout(box);

  shell_view_ = CreateView(BrowserRole::kShell, 0, InternalPageUrl("shell"), &shell_client_);
  window->AddChildView(shell_view_);
  layout->SetFlexForView(shell_view_, 0);

  content_ = CefPanel::CreatePanel(nullptr);
  content_->SetToFillLayout();
  window->AddChildView(content_);
  layout->SetFlexForView(content_, 1);

  if (first_url_.empty()) {
    AddTab(BrowserRole::kInternalTab, InternalPageUrl("newtab"), true);
  } else {
    AddTab(BrowserRole::kWebTab, first_url_, true);
  }

  if (services.settings) {
    CefRefPtr<BrowserWindow> self = this;
    settings_observer_ = services.settings->AddObserver([self](const std::string& key) {
      if (key == "appearance.theme") {
        self->ApplyTheme();
      }
    });
  }
  window->SetTitle("TorFox");
  window->CenterWindow(GetPreferredSize(window));
  window->Layout();
  {
    auto b = [](CefRefPtr<CefView> v) {
      CefRect r = v ? v->GetBounds() : CefRect();
      return Json::array({r.x, r.y, r.width, r.height});
    };
    Tab* t = ActiveTab();
    test_log::Event("window_layout", {{"window", b(window)}, {"shell", b(shell_view_)},
                                      {"content", b(content_)},
                                      {"tab", b(t ? CefRefPtr<CefView>(t->view) : nullptr)},
                                      {"tab_visible", t && t->view->IsVisible()},
                                      {"content_children", content_->GetChildViewCount()}});
  }
  window->Show();
  ApplyTheme();
  test_log::Event("window_created");
}

void BrowserWindow::ApplyTheme() {
  if (!window_) {
    return;
  }
  Services& services = GetServices();
  const bool dark = !services.settings || services.settings->EffectiveDark();
  window_->SetBackgroundColor(dark ? kDarkBackground : kLightBackground);
  if (content_) {
    content_->SetBackgroundColor(dark ? kDarkBackground : kLightBackground);
  }
  platform::ApplyWindowTheme(window_, dark);
}

CefSize BrowserWindow::GetPreferredSize(CefRefPtr<CefView> view) {
  return CefSize(1280, 820);
}

CefSize BrowserWindow::GetMinimumSize(CefRefPtr<CefView> view) {
  return CefSize(480, 320);
}

cef_runtime_style_t BrowserWindow::GetWindowRuntimeStyle() {
  return CEF_RUNTIME_STYLE_ALLOY;
}

// ----- Tabs ------------------------------------------------------------------

int BrowserWindow::AddTab(BrowserRole role, const std::string& url, bool select) {
  Tab tab;
  tab.id = next_tab_id_++;
  tab.url = url;
  tab.title = role == BrowserRole::kWebTab ? url : "New Tab";
  tab.view = CreateView(role, tab.id, url, &tab.client);
  content_->AddChildView(tab.view);
  tab.view->SetVisible(false);
  const int id = tab.id;
  tabs_.push_back(std::move(tab));
  if (select || active_tab_id_ == 0) {
    Activate(id);
  } else {
    PushState();
  }
  return id;
}

void BrowserWindow::NewTab() {
  if (closing_window_ || !content_) {
    return;
  }
  AddTab(BrowserRole::kInternalTab, InternalPageUrl("newtab"), true);
  HandleShortcut(Shortcut::kFocusAddress);
}

void BrowserWindow::OpenWebUrlInNewTab(const std::string& url, bool foreground) {
  if (closing_window_ || !content_) {
    return;
  }
  AddTab(BrowserRole::kWebTab, url, foreground);
}

BrowserWindow::Result BrowserWindow::CloseTab(int tab_id) {
  auto it = std::find_if(tabs_.begin(), tabs_.end(), [tab_id](const Tab& t) { return t.id == tab_id; });
  if (it == tabs_.end()) {
    return {false, "no such tab"};
  }
  const std::size_t index = static_cast<std::size_t>(it - tabs_.begin());
  Tab tab = std::move(*it);
  tabs_.erase(it);
  StartClose(tab.client, tab.view, content_);

  if (tabs_.empty()) {
    // Closing the last tab closes the window.
    active_tab_id_ = 0;
    if (window_) {
      window_->Close();
    }
    return {};
  }
  if (active_tab_id_ == tab_id) {
    const std::size_t next = std::min(index, tabs_.size() - 1);
    active_tab_id_ = 0;
    Activate(tabs_[next].id);
  } else {
    PushState();
  }
  return {};
}

BrowserWindow::Result BrowserWindow::SelectTab(int tab_id) {
  if (!FindTab(tab_id)) {
    return {false, "no such tab"};
  }
  Activate(tab_id);
  return {};
}

void BrowserWindow::Activate(int tab_id) {
  for (Tab& t : tabs_) {
    t.view->SetVisible(t.id == tab_id);
  }
  active_tab_id_ = tab_id;
  FocusActiveContent();
  UpdateTitle();
  PushState();
}

void BrowserWindow::FocusActiveContent() {
  if (Tab* t = ActiveTab()) {
    t->view->RequestFocus();
  }
}

BrowserWindow::Tab* BrowserWindow::FindTab(int tab_id) {
  for (Tab& t : tabs_) {
    if (t.id == tab_id) {
      return &t;
    }
  }
  return nullptr;
}

BrowserWindow::Tab* BrowserWindow::ActiveTab() {
  return FindTab(active_tab_id_);
}

CefRefPtr<CefBrowser> BrowserWindow::ActiveBrowser() {
  Tab* t = ActiveTab();
  return t && t->client ? t->client->browser() : nullptr;
}

// ----- Navigation ------------------------------------------------------------

BrowserWindow::Result BrowserWindow::Navigate(int tab_id, const std::string& input) {
  Tab* tab = tab_id == 0 ? ActiveTab() : FindTab(tab_id);
  if (!tab) {
    return {false, "no such tab"};
  }
  NavDecision d = ClassifyNavInput(input);
  test_log::Event("nav_input", {{"kind", static_cast<int>(d.kind)}, {"tab", tab->id}});
  switch (d.kind) {
    case NavKind::kRejected:
      return {false, d.reason};
    case NavKind::kOnion:
      // Never searched, never loaded outside Tor. Tor tabs arrive in Stage 5.
      return {false, "Onion sites open only in Tor tabs, and Tor support is not available yet."};
    case NavKind::kInternal:
      LoadInTab(*tab, BrowserRole::kInternalTab, d.url);
      return {};
    case NavKind::kWebUrl:
    case NavKind::kSearch:
      LoadInTab(*tab, BrowserRole::kWebTab, d.url);
      return {};
  }
  return {false, "unsupported input"};
}

void BrowserWindow::LoadInTab(Tab& tab, BrowserRole role, const std::string& url) {
  tab.url = url;
  if (tab.client && tab.client->role() == role && tab.client->browser()) {
    tab.client->browser()->GetMainFrame()->LoadURL(url);
  } else {
    // Changing kind (internal <-> web) replaces the browser: fresh renderer,
    // different request context, no shared state or history.
    CefRefPtr<TabClient> old_client = tab.client;
    CefRefPtr<CefBrowserView> old_view = tab.view;
    tab.view = CreateView(role, tab.id, url, &tab.client);
    tab.title = role == BrowserRole::kWebTab ? url : "New Tab";
    tab.loading = true;
    tab.can_back = tab.can_forward = false;
    content_->AddChildView(tab.view);
    tab.view->SetVisible(tab.id == active_tab_id_);
    StartClose(old_client, old_view, content_);
  }
  if (tab.id == active_tab_id_) {
    FocusActiveContent();
  }
  UpdateTitle();
  PushState();
}

void BrowserWindow::GoBack() {
  if (auto b = ActiveBrowser()) {
    b->GoBack();
  }
}

void BrowserWindow::GoForward() {
  if (auto b = ActiveBrowser()) {
    b->GoForward();
  }
}

void BrowserWindow::Reload() {
  if (auto b = ActiveBrowser()) {
    b->Reload();
  }
}

void BrowserWindow::Stop() {
  if (auto b = ActiveBrowser()) {
    b->StopLoad();
  }
}

void BrowserWindow::OpenSettings() {
  const std::string url = InternalPageUrl("settings");
  for (Tab& t : tabs_) {
    if (t.client && t.client->role() == BrowserRole::kInternalTab && t.url == url) {
      Activate(t.id);
      return;
    }
  }
  AddTab(BrowserRole::kInternalTab, url, true);
}

void BrowserWindow::HandleShortcut(Shortcut shortcut) {
  if (closing_window_) {
    return;
  }
  switch (shortcut) {
    case Shortcut::kNewTab:
      NewTab();
      break;
    case Shortcut::kCloseTab:
      if (active_tab_id_) {
        CloseTab(active_tab_id_);
      }
      break;
    case Shortcut::kFocusAddress:
      if (shell_view_) {
        shell_view_->RequestFocus();
      }
      PushEvent({{"type", "focusAddress"}});
      break;
    case Shortcut::kReload:
      Reload();
      break;
    case Shortcut::kBack:
      GoBack();
      break;
    case Shortcut::kForward:
      GoForward();
      break;
    case Shortcut::kNextTab:
    case Shortcut::kPreviousTab: {
      if (tabs_.size() < 2) {
        break;
      }
      auto it = std::find_if(tabs_.begin(), tabs_.end(),
                             [this](const Tab& t) { return t.id == active_tab_id_; });
      std::size_t i = it == tabs_.end() ? 0 : static_cast<std::size_t>(it - tabs_.begin());
      i = shortcut == Shortcut::kNextTab ? (i + 1) % tabs_.size()
                                         : (i + tabs_.size() - 1) % tabs_.size();
      Activate(tabs_[i].id);
      break;
    }
    case Shortcut::kSettings:
      OpenSettings();
      break;
  }
}

// ----- Client callbacks ------------------------------------------------------

void BrowserWindow::OnClientCreated(TabClient* client, CefRefPtr<CefBrowser> browser) {
  if (client == shell_client_.get()) {
    test_log::Event("shell_created", {{"browser", browser->GetIdentifier()}});
  }
}

void BrowserWindow::StartClose(CefRefPtr<TabClient> client,
                               CefRefPtr<CefBrowserView> view,
                               CefRefPtr<CefPanel> parent) {
  if (!client) {
    return;
  }
  closing_[client.get()] = Closing{view, parent};
  client->Close();
}

void BrowserWindow::OnClientDoClose(TabClient* client) {
  if (closing_.find(client) == closing_.end()) {
    // A close TorFox did not start (unexpected: window.close() is disabled
    // for web content). Detach that tab so the close can complete.
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                           [client](const Tab& t) { return t.client.get() == client; });
    if (it == tabs_.end()) {
      return;
    }
    const int tab_id = it->id;
    closing_[client] = Closing{it->view, content_};
    tabs_.erase(it);
    if (tabs_.empty()) {
      active_tab_id_ = 0;
      if (window_) {
        window_->Close();
      }
    } else if (active_tab_id_ == tab_id) {
      active_tab_id_ = 0;
      Activate(tabs_.front().id);
    } else {
      PushState();
    }
  }
  // Remove the view and drop our references to it on the next task, once CEF
  // has finished processing DoClose.
  CefRefPtr<BrowserWindow> self = this;
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<BrowserWindow> self, CefRefPtr<TabClient> client) {
                            auto it = self->closing_.find(client.get());
                            if (it == self->closing_.end()) {
                              return;
                            }
                            // Take the references out first: removing the view
                            // can finish the close synchronously, and
                            // OnClientClosed then erases this map entry.
                            CefRefPtr<CefBrowserView> view = std::move(it->second.view);
                            CefRefPtr<CefPanel> parent = std::move(it->second.parent);
                            it->second.view = nullptr;
                            it->second.parent = nullptr;
                            if (parent && view) {
                              parent->RemoveChildView(view);
                            }
                            view = nullptr;
                            parent = nullptr;
                          },
                          self, CefRefPtr<TabClient>(client)));
}

void BrowserWindow::OnClientClosed(TabClient* client) {
  closing_.erase(client);
  if (client == shell_client_.get()) {
    shell_client_ = nullptr;
    shell_view_ = nullptr;
  }
  for (auto it = state_subscribers_.begin(); it != state_subscribers_.end();) {
    auto entry = Registry().Find(it->browser_id);
    it = entry ? std::next(it) : state_subscribers_.erase(it);
  }
  --live_clients_;
  if (closing_window_ && live_clients_ == 0 && window_) {
    CefRefPtr<CefWindow> window = window_;
    CefPostTask(TID_UI, base::BindOnce(&CefWindow::Close, window));
  }
}

void BrowserWindow::OnTitleChange(int tab_id, const std::string& title) {
  if (Tab* t = FindTab(tab_id)) {
    t->title = title;
    if (tab_id == active_tab_id_) {
      UpdateTitle();
    }
    PushState();
  }
}

void BrowserWindow::OnAddressChange(int tab_id, const std::string& url) {
  if (Tab* t = FindTab(tab_id)) {
    t->url = url;
    PushState();
  }
}

void BrowserWindow::OnLoadingState(int tab_id, bool loading, bool can_back, bool can_forward) {
  if (Tab* t = FindTab(tab_id)) {
    t->loading = loading;
    t->can_back = can_back;
    t->can_forward = can_forward;
    PushState();
  }
}

void BrowserWindow::OnRendererCrashed(TabClient* client) {
  if (client == shell_client_.get() && client->browser() && !closing_window_) {
    // The shell must always work: reload it.
    client->browser()->GetMainFrame()->LoadURL(InternalPageUrl("shell"));
  }
}

// ----- Window close ----------------------------------------------------------

bool BrowserWindow::CanClose(CefRefPtr<CefWindow> window) {
  if (live_clients_ == 0) {
    return true;
  }
  if (!closing_window_) {
    closing_window_ = true;
    std::vector<Tab> tabs = std::move(tabs_);
    tabs_.clear();
    active_tab_id_ = 0;
    for (Tab& t : tabs) {
      StartClose(t.client, t.view, content_);
    }
    // Hand the shell's view to the close sequence and drop our own reference:
    // a browser whose view is still referenced never finishes closing
    // (docs/STAGE0_FINDINGS.md, spike B).
    CefRefPtr<CefBrowserView> shell_view = std::move(shell_view_);
    shell_view_ = nullptr;
    StartClose(shell_client_, shell_view, window_);
  }
  return false;
}

void BrowserWindow::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  Services& services = GetServices();
  if (services.settings && settings_observer_) {
    services.settings->RemoveObserver(settings_observer_);
    settings_observer_ = 0;
  }
  state_subscribers_.clear();
  closing_.clear();
  tabs_.clear();
  content_ = nullptr;
  shell_view_ = nullptr;
  shell_client_ = nullptr;
  window_ = nullptr;
  test_log::Event("window_destroyed");
  if (services.window_destroyed) {
    services.window_destroyed(this);
  }
}

// ----- State for the shell ---------------------------------------------------

void BrowserWindow::UpdateTitle() {
  if (!window_) {
    return;
  }
  Tab* t = ActiveTab();
  std::string title = t && !t->title.empty() ? t->title + " \xE2\x80\x94 TorFox" : "TorFox";
  if (kTestHooks) {
    title += " (TEST BUILD)";  // Test-hook builds must never pass for a release.
  }
  window_->SetTitle(title);
}

Json BrowserWindow::StateJson() const {
  Json tabs = Json::array();
  Json nav = Json::object();
  for (const Tab& t : tabs_) {
    const bool internal = t.client && IsInternalRole(t.client->role());
    tabs.push_back({{"id", t.id},
                    {"title", t.title},
                    {"url", t.url},
                    {"internal", internal},
                    {"loading", t.loading}});
    if (t.id == active_tab_id_) {
      nav = {{"url", t.url},
             {"internal", internal},
             {"loading", t.loading},
             {"canGoBack", t.can_back},
             {"canGoForward", t.can_forward}};
    }
  }
  return {{"type", "state"}, {"tabs", tabs}, {"activeId", active_tab_id_}, {"nav", nav}};
}

void BrowserWindow::PushEvent(const Json& event) {
  const std::string payload = DumpJson(event);
  for (Subscriber& s : state_subscribers_) {
    s.callback->Success(payload);
  }
}

void BrowserWindow::PushState() {
  if (!state_subscribers_.empty()) {
    PushEvent(StateJson());
  }
}

void BrowserWindow::AddStateSubscriber(int browser_id,
                                       int64_t query_id,
                                       CefRefPtr<CefMessageRouterBrowserSide::Callback> callback) {
  state_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
  callback->Success(DumpJson(StateJson()));
}

void BrowserWindow::RemoveStateSubscriber(int browser_id, int64_t query_id) {
  state_subscribers_.erase(
      std::remove_if(state_subscribers_.begin(), state_subscribers_.end(),
                     [&](const Subscriber& s) {
                       return s.browser_id == browser_id && s.query_id == query_id;
                     }),
      state_subscribers_.end());
}

}  // namespace torfox
