// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/browser_window.h"

#include <algorithm>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_fill_layout.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/app_common.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/download_service.h"
#include "src/browser/app/places_service.h"
#include "src/browser/app/privacy_service.h"
#include "src/browser/app/services.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/browser/app/tor_service.h"
#include "src/browser/app/update_service.h"
#include "src/browser/platform/platform.h"
#include "src/browser/shell/dialogs.h"
#include "src/browser/shell/menus.h"
#include "src/browser/shell/tab_client.h"
#include "src/core/bridge/validator.h"
#include "src/core/browsing/context_menu.h"
#include "src/core/browsing/zoom.h"
#include "src/core/tor/onion.h"
#include "src/core/url/guards.h"
#include "src/core/url/internal_url.h"
#include "src/core/url/nav_input.h"

namespace torfox {

namespace {

// DIP heights of the shell's rows; must match ui/src/shell/shell.ts.
constexpr int kStripHeight = 38;
constexpr int kToolbarHeight = 44;
constexpr int kBookmarksBarHeight = 32;
constexpr int kFindBarHeight = 40;

constexpr cef_color_t kDarkBackground = 0xFF1C1B22;
constexpr cef_color_t kLightBackground = 0xFFF6F5F9;

constexpr char kBookmarksBarSetting[] = "appearance.showBookmarksBar";
constexpr char kTorLocalRefused[] =
    "Tor windows cannot open addresses on this computer or your local network.";

// Native menu command ids.
constexpr int kAppMenuBase = 1000;  // + browsing::Command
constexpr int kStarRemove = 1;
constexpr int kStarEdit = 2;
constexpr int kShieldAllow = 3;
constexpr int kShieldBlock = 4;
constexpr int kShieldSettings = 5;
constexpr int kBookmarkMenuEmpty = 90000;
constexpr int kBookmarkSubmenuBase = 100000;
constexpr std::size_t kMaxBookmarkMenuItems = 500;
constexpr int kMaxBookmarkMenuDepth = 6;
constexpr std::size_t kMaxBookmarksBarItems = 200;

class ViewDelegate : public CefBrowserViewDelegate {
 public:
  ViewDelegate(BrowserRole role, std::shared_ptr<int> shell_height)
      : role_(role), shell_height_(std::move(shell_height)) {}

  // The shell's height follows its rows (bookmarks bar, find bar); the
  // BoxLayout stretches it horizontally. CEF treats a size with any zero
  // dimension as "use the default", so the width must be non-zero. Tab views
  // keep the defaults and are sized by the content panel's FillLayout.
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return IsShell() ? CefSize(1, *shell_height_) : CefSize();
  }
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
    return IsShell() ? CefSize(1, *shell_height_) : CefSize();
  }
  int GetHeightForWidth(CefRefPtr<CefView> view, int width) override {
    return IsShell() ? *shell_height_ : 0;
  }
  cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

 private:
  bool IsShell() const { return role_ == BrowserRole::kShell && shell_height_; }

  const BrowserRole role_;
  std::shared_ptr<int> shell_height_;
  IMPLEMENT_REFCOUNTING(ViewDelegate);
};

bool IsInternalTabUrl(const std::string& url) {
  auto parsed = ParseInternalUrl(url);
  return parsed && IsTabPage(parsed->host);
}

// The suggestion list's rows and frame (ui/src/suggest/main.ts): 32 px rows
// inside 4 px of padding and a 1 px border, above and below.
constexpr int kSuggestRowHeight = 32;
constexpr int kSuggestFrameHeight = 2 * 4 + 2 * 1;

// Menu labels: no mnemonics from page-controlled text, and kept short.
std::string MenuLabel(const std::string& text) {
  std::string excerpt = browsing::MenuExcerpt(text, 60);
  std::string out;
  for (char c : excerpt) {
    out += c;
    if (c == '&') {
      out += '&';
    }
  }
  return out;
}

PlacesService* Places() {
  return GetServices().places;
}

browsing::Places* PlacesDb() {
  PlacesService* ps = Places();
  return ps ? ps->places() : nullptr;
}

}  // namespace

// ----- Creation --------------------------------------------------------------

void BrowserWindow::Open(const std::string& first_url, bool private_window,
                         CefRefPtr<BrowserWindow> requester) {
  CEF_REQUIRE_UI_THREAD();
  Services& services = GetServices();
  if (!services.contexts) {
    return;
  }
  if (!private_window) {
    CefWindow::CreateTopLevelWindow(new BrowserWindow(
        first_url, false, false, services.contexts->Get(ContextManager::Kind::kNormal)));
    return;
  }
  // A private window may only exist once its own context is verified.
  services.contexts->CreatePrivate(
      [first_url, requester](CefRefPtr<CefRequestContext> context, const std::string& error) {
        if (!context) {
          test_log::Event("private_window_failed", {{"error", error}});
          if (requester) {
            requester->ShowNotice("TorFox could not open a private window: " + error);
          }
          return;
        }
        CefWindow::CreateTopLevelWindow(new BrowserWindow(first_url, true, false, context));
      });
}

void BrowserWindow::OpenTor(const std::string& first_url,
                            CefRefPtr<BrowserWindow> requester,
                            std::function<void(CefRefPtr<BrowserWindow>)> opened) {
  CEF_REQUIRE_UI_THREAD();
  Services& services = GetServices();
  auto failed = [requester, opened](const std::string& error) {
    test_log::Event("tor_window_failed", {{"error", error}});
    if (requester) {
      requester->ShowNotice("TorFox could not open a Tor window: " + error);
    }
    if (opened) {
      opened(nullptr);
    }
  };
  if (!services.contexts || !services.tor) {
    failed("Tor is not available.");
    return;
  }
  if (services.tor_start) {
    services.tor_start();
  }
  services.contexts->CreateTor(
      0, [first_url, failed, opened](CefRefPtr<CefRequestContext> context, const std::string& error) {
        if (!context) {
          failed(error);
          return;
        }
        CefRefPtr<BrowserWindow> window = new BrowserWindow(first_url, true, true, context);
        CefWindow::CreateTopLevelWindow(window);
        if (opened) {
          opened(window);
        }
      });
}

void BrowserWindow::OpenRestored(std::vector<std::string> urls, std::size_t active,
                                 const std::string& extra_url) {
  CEF_REQUIRE_UI_THREAD();
  Services& services = GetServices();
  if (!services.contexts) {
    return;
  }
  CefRefPtr<BrowserWindow> window = new BrowserWindow(
      extra_url, false, false, services.contexts->Get(ContextManager::Kind::kNormal));
  window->restore_urls_ = std::move(urls);
  window->restore_active_ = active;
  CefWindow::CreateTopLevelWindow(window);
}

browsing::SessionWindow BrowserWindow::SessionTabs() const {
  browsing::SessionWindow w;
  for (const Tab& t : tabs_) {
    if (t.id == active_tab_id_) {
      w.active = w.urls.size();
    }
    w.urls.push_back(t.url);
  }
  return w;
}

void BrowserWindow::Close() {
  if (window_ && !closing_window_) {
    window_->Close();
  }
}

BrowserWindow::~BrowserWindow() = default;

BrowserWindow::BrowserWindow(std::string first_url, bool private_window, bool tor,
                             CefRefPtr<CefRequestContext> web_context)
    : first_url_(std::move(first_url)),
      private_(private_window || tor),
      tor_(tor),
      id_([] {
        static int next_id = 1;
        return next_id++;
      }()),
      web_context_(std::move(web_context)),
      tor_retired_(tor && GetServices().contexts ? GetServices().contexts->TorRetired(web_context_)
                                                 : nullptr),
      shell_height_(std::make_shared<int>(kStripHeight + kToolbarHeight)),
      ads_allowed_(!private_ && GetServices().privacy ? GetServices().privacy->normal_ads_allowed()
                                                      : std::make_shared<adblock::SiteList>()) {}

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
  } else if (tor_) {
    extra = CefDictionaryValue::Create();
    extra->SetBool(kExtraInfoTor, true);
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
  CefRefPtr<CefRequestContext> context =
      internal ? services.contexts->Get(ContextManager::Kind::kInternal) : web_context_;
  // Created without a URL: TabClient loads |url| once the browser is in the
  // registry, so the very first request is already subject to policy.
  CefRefPtr<CefBrowserView> view = CefBrowserView::CreateBrowserView(
      client, "", settings, extra, context,
      new ViewDelegate(role, role == BrowserRole::kShell ? shell_height_ : nullptr));
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
  *shell_height_ = kStripHeight + kToolbarHeight + (BookmarksBarVisible() ? kBookmarksBarHeight : 0);

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

  // The first tab always starts as the new-tab page. A web address given
  // for it is loaded once the window is shown (below): a web browser created
  // before its window is shown stays "hidden" to Chromium (pages see
  // document.visibilityState "hidden", timers are throttled, and permission
  // requests wait forever), found by test_stage1_defaults.py.
  AddTab(BrowserRole::kInternalTab, InternalPageUrl("newtab"), true);

  CefRefPtr<BrowserWindow> self = this;
  if (services.settings) {
    settings_observer_ = services.settings->AddObserver([self](const std::string& key) {
      if (key == "appearance.theme") {
        self->ApplyTheme();
      } else if (key == kBookmarksBarSetting) {
        self->RelayoutShell();
        self->PushBookmarksBar();
        self->PushState();
      }
    });
  }
  if (services.places) {
    places_observer_ = services.places->AddObserver([self](PlacesService::Change change) {
      if (change == PlacesService::Change::kBookmarks) {
        self->PushState();  // The star.
        self->PushBookmarksBar();
      }
    });
  }
  if (services.downloads) {
    downloads_observer_ = services.downloads->AddObserver([self]() {
      // Downloads were just disabled: no download prompt may stay open.
      if (DownloadService* d = GetServices().downloads;
          d && d->Decide() == downloads::Decision::kBlock) {
        self->CancelPrompts(0, "download");
      }
      self->PushState();
    });
  }
  if (tor_ && services.tor) {
    tor_observer_ = services.tor->AddObserver([self]() { self->OnTorStatus(); });
  }
  window->SetTitle(tor_ ? "TorFox Tor" : "TorFox");
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
                                      {"content_children", content_->GetChildViewCount()},
                                      {"private", private_}});
  }
  window->Show();
  ApplyTheme();
  test_log::Event("window_created", {{"private", private_}, {"tor", tor_}, {"id", id_}});
  // Tabs for a saved session or a start address are created once the window
  // is on screen: browsers created while it is still being shown stay
  // "hidden" to Chromium (found by test_startup.py).
  CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::OpenFirstTabs, self));
}

void BrowserWindow::OpenFirstTabs() {
  if (closing_window_ || !content_) {
    return;
  }
  if (!restore_urls_.empty()) {
    // A saved session: its first tab replaces the new-tab page, the others
    // open behind it.
    const int first_id = active_tab_id_;
    for (std::size_t i = 0; i < restore_urls_.size(); ++i) {
      const std::string& url = restore_urls_[i];
      const BrowserRole role = IsInternalTabUrl(url) ? BrowserRole::kInternalTab : BrowserRole::kWebTab;
      if (i == 0) {
        if (Tab* first = FindTab(first_id); first && url != first->url) {
          LoadInTab(*first, role, url);
        }
      } else {
        AddTab(role, url, false);
      }
    }
    if (restore_active_ < tabs_.size()) {
      Activate(tabs_[restore_active_].id);
    }
    test_log::Event("session_window_restored", {{"id", id_}, {"tabs", restore_urls_.size()}});
    restore_urls_.clear();
    if (!first_url_.empty()) {
      AddTab(BrowserRole::kWebTab, first_url_, true);
    }
  } else if (!first_url_.empty()) {
    if (Tab* first = ActiveTab()) {
      LoadInTab(*first, BrowserRole::kWebTab, first_url_);
    }
  }
  // After an update: the first normal window to open shows What's new, in
  // front. Never a private or Tor window.
  Services& services = GetServices();
  if (services.show_whats_new && !private_) {
    services.show_whats_new = false;
    test_log::Event("whats_new_opened", {{"window", id_}, {"from", services.whats_new_from}});
    OpenInternalPage("whatsnew");
  }
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

bool BrowserWindow::BookmarksBarVisible() const {
  Services& services = GetServices();
  return services.settings && services.settings->store().GetBool(kBookmarksBarSetting);
}

void BrowserWindow::RelayoutShell() {
  const int height = kStripHeight + kToolbarHeight +
                     (BookmarksBarVisible() ? kBookmarksBarHeight : 0) +
                     (find_open_ ? kFindBarHeight : 0);
  if (*shell_height_ == height || !window_) {
    return;
  }
  *shell_height_ = height;
  if (shell_view_) {
    shell_view_->InvalidateLayout();
  }
  window_->Layout();
  CefRect shell = shell_view_ ? shell_view_->GetBounds() : CefRect();
  CefRect content = content_ ? content_->GetBounds() : CefRect();
  test_log::Event("shell_layout", {{"height", height},
                                   {"shell", Json::array({shell.y, shell.height})},
                                   {"content", Json::array({content.y, content.height})}});
}

// ----- Tabs ------------------------------------------------------------------

int BrowserWindow::AddTab(BrowserRole role, const std::string& url, bool select, int index,
                          int opener) {
  Tab tab;
  tab.id = next_tab_id_++;
  tab.opener = opener;
  tab.url = url;
  tab.title = role == BrowserRole::kWebTab ? url : "New Tab";
  tab.view = CreateView(role, tab.id, url, &tab.client);
  content_->AddChildView(tab.view);
  tab.view->SetVisible(false);
  const int id = tab.id;
  const std::size_t pos =
      index < 0 ? tabs_.size() : std::min(static_cast<std::size_t>(index), tabs_.size());
  tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(tab));
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
  HandleCommand({browsing::Command::kFocusAddress});
}

void BrowserWindow::OpenWebUrlInNewTab(std::string url, bool foreground, int opener_tab_id) {
  if (closing_window_ || !content_) {
    return;
  }
  std::vector<browsing::TabRef> refs;
  for (const Tab& t : tabs_) {
    refs.push_back({t.id, t.opener});
  }
  const std::size_t index = browsing::ChildInsertIndex(refs, opener_tab_id);
  AddTab(BrowserRole::kWebTab, url, foreground, static_cast<int>(index), opener_tab_id);
}

int BrowserWindow::IndexOf(int tab_id) const {
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].id == tab_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

BrowserWindow::Result BrowserWindow::CloseTab(int tab_id) {
  const int pos = IndexOf(tab_id);
  if (pos < 0) {
    return {false, "no such tab"};
  }
  const std::size_t index = static_cast<std::size_t>(pos);
  CancelPrompts(tab_id);
  // A tab's downloads end with it (a CEF limit): say so.
  int cancelled_downloads = 0;
  if (DownloadService* d = GetServices().downloads) {
    cancelled_downloads = d->CancelForTab(id_, tab_id, "tab closed");
  }
  Tab tab = std::move(tabs_[index]);
  tabs_.erase(tabs_.begin() + pos);
  if (find_open_ && find_tab_id_ == tab_id) {
    find_open_ = false;
    find_text_.clear();
    RelayoutShell();
  }
  // Remembered for "Reopen closed tab" while this window lives (in memory
  // only; private windows' lists die with the window).
  if (browsing::IsStorableUrl(tab.url) || IsInternalTabUrl(tab.url)) {
    closed_tabs_.Push({tab.url, tab.title, index});
  }
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
    active_tab_id_ = 0;
    Activate(tabs_[browsing::NextActiveIndex(index, tabs_.size())].id);
  } else {
    PushState();
  }
  if (cancelled_downloads > 0) {
    ShowNotice(cancelled_downloads == 1
                   ? "Closing the tab cancelled its download."
                   : "Closing the tab cancelled its downloads.");
  }
  return {};
}

void BrowserWindow::ReopenClosedTab() {
  if (closing_window_ || !content_) {
    return;
  }
  auto entry = closed_tabs_.Pop();
  if (!entry) {
    return;
  }
  const BrowserRole role =
      IsInternalTabUrl(entry->url) ? BrowserRole::kInternalTab : BrowserRole::kWebTab;
  const std::size_t index = browsing::ClampIndex(static_cast<long long>(entry->index),
                                                 tabs_.size() + 1);
  AddTab(role, entry->url, true, static_cast<int>(index));
  test_log::Event("tab_reopened", {{"index", index}});
}

BrowserWindow::Result BrowserWindow::SelectTab(int tab_id) {
  if (!FindTab(tab_id)) {
    return {false, "no such tab"};
  }
  Activate(tab_id);
  return {};
}

BrowserWindow::Result BrowserWindow::MoveTab(int tab_id, int index) {
  const int from = IndexOf(tab_id);
  if (from < 0) {
    return {false, "no such tab"};
  }
  const int to = static_cast<int>(browsing::ClampIndex(index, tabs_.size()));
  if (from != to) {
    Tab tab = std::move(tabs_[static_cast<std::size_t>(from)]);
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + to, std::move(tab));
    // A moved tab is no longer grouped with its opener.
    tabs_[static_cast<std::size_t>(to)].opener = 0;
    PushState();
  }
  return {};
}

void BrowserWindow::MoveActiveTab(int delta) {
  const int from = IndexOf(active_tab_id_);
  const int to = from + delta;
  if (from >= 0 && to >= 0 && to < static_cast<int>(tabs_.size())) {
    MoveTab(active_tab_id_, to);
  }
}

void BrowserWindow::SelectTabNumber(int number) {
  if (tabs_.empty() || number < 1 || number > 9) {
    return;
  }
  const std::size_t index = number == 9 ? tabs_.size() - 1 : static_cast<std::size_t>(number - 1);
  if (index < tabs_.size()) {
    Activate(tabs_[index].id);
  }
}

void BrowserWindow::Activate(int tab_id) {
  SuggestHide();
  if (find_open_ && find_tab_id_ != tab_id) {
    // The find bar belongs to the tab it was opened in.
    if (Tab* old = FindTab(find_tab_id_); old && old->client && old->client->browser()) {
      old->client->browser()->GetHost()->StopFinding(true);
    }
    find_open_ = false;
    find_text_.clear();
    RelayoutShell();
  }
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
    case NavKind::kOnion: {
      // Never searched, never loaded outside Tor: from other windows it
      // opens in a Tor window.
      const tor::OnionInput onion = tor::OnionUrlFromInput(d.url);
      if (!onion.error.empty()) {
        return {false, onion.error};
      }
      if (!tor_) {
        OpenTor(onion.url, this);
        return {};
      }
      LoadInTab(*tab, BrowserRole::kWebTab, onion.url);
      return {};
    }
    case NavKind::kInternal:
      LoadInTab(*tab, BrowserRole::kInternalTab, d.url);
      return {};
    case NavKind::kWebUrl:
    case NavKind::kSearch: {
      std::string url = d.url;
      if (d.kind == NavKind::kSearch) {
        // Settings > Search; DuckDuckGo's onion service in Tor windows.
        url = SearchUrlFor(url, tor_);
      }
      if (tor_) {
        if (!guards::NavigationBlockReason(guards::Role::kTorTab, url).empty()) {
          return {false, kTorLocalRefused};
        }
      }
      LoadInTab(*tab, BrowserRole::kWebTab, url);
      return {};
    }
  }
  return {false, "unsupported input"};
}

BrowserWindow::Result BrowserWindow::OpenUrl(int tab_id, const std::string& url, bool new_tab) {
  // Only stored web URLs; then the same classification as typed input, so an
  // onion URL can never be opened outside Tor this way either.
  if (!browsing::IsStorableUrl(url)) {
    return {false, "This address cannot be opened."};
  }
  NavDecision d = ClassifyNavInput(url);
  if (d.kind == NavKind::kOnion) {
    const tor::OnionInput onion = tor::OnionUrlFromInput(d.url);
    if (!onion.error.empty()) {
      return {false, onion.error};
    }
    if (!tor_) {
      OpenTor(onion.url, this);
      return {};
    }
    d.kind = NavKind::kWebUrl;
    d.url = onion.url;
  }
  if (tor_ && !guards::NavigationBlockReason(guards::Role::kTorTab, d.url).empty()) {
    return {false, kTorLocalRefused};
  }
  if (d.kind != NavKind::kWebUrl) {
    return {false, "This address cannot be opened."};
  }
  Tab* tab = tab_id == 0 ? ActiveTab() : FindTab(tab_id);
  if (new_tab || !tab) {
    OpenWebUrlInNewTab(d.url, true, tab ? tab->id : 0);
    return {};
  }
  LoadInTab(*tab, BrowserRole::kWebTab, d.url);
  return {};
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

void BrowserWindow::Reload(bool bypass_cache) {
  if (auto b = ActiveBrowser()) {
    if (bypass_cache) {
      b->ReloadIgnoreCache();
    } else {
      b->Reload();
    }
  }
}

void BrowserWindow::Stop() {
  if (auto b = ActiveBrowser()) {
    b->StopLoad();
  }
}

void BrowserWindow::Home() {
  if (Tab* t = ActiveTab()) {
    LoadInTab(*t, BrowserRole::kInternalTab, InternalPageUrl("newtab"));
  }
}

void BrowserWindow::OpenSettings() {
  OpenInternalPage("settings");
}

void BrowserWindow::OpenInternalPage(std::string_view page) {
  if (closing_window_ || !content_ || !IsTabPage(page)) {
    return;
  }
  const std::string url = InternalPageUrl(page);
  for (Tab& t : tabs_) {
    auto parsed = ParseInternalUrl(t.url);
    if (t.client && t.client->role() == BrowserRole::kInternalTab && parsed &&
        parsed->host == page) {
      Activate(t.id);
      return;
    }
  }
  AddTab(BrowserRole::kInternalTab, url, true);
}

// ----- Find ------------------------------------------------------------------

void BrowserWindow::Find(const std::string& text, bool forward, bool match_case) {
  Tab* t = ActiveTab();
  if (!find_open_ || !t || !t->client || !t->client->browser()) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = t->client->browser()->GetHost();
  if (text.empty()) {
    host->StopFinding(true);
    find_text_.clear();
    PushEvent({{"type", "find"}, {"count", 0}, {"active", 0}, {"final", true}});
    return;
  }
  find_tab_id_ = t->id;
  find_text_ = text;
  find_match_case_ = match_case;
  // Always a "follow-up": CEF restarts the search itself when the text or
  // case changes, and only a follow-up selects a match (with findNext=false
  // every match is highlighted but none becomes active, so Enter would skip
  // the first one).
  host->Find(text, forward, match_case, true);
}

void BrowserWindow::StopFind() {
  if (!find_open_) {
    return;
  }
  if (Tab* t = FindTab(find_tab_id_); t && t->client && t->client->browser()) {
    t->client->browser()->GetHost()->StopFinding(true);
  }
  find_open_ = false;
  find_text_.clear();
  RelayoutShell();
  PushState();
  FocusActiveContent();
}

void BrowserWindow::OnFindResult(int tab_id, int count, int active, bool final_update) {
  if (!find_open_ || tab_id != find_tab_id_) {
    return;
  }
  if (final_update) {
    test_log::Event("find_result", {{"count", count}, {"active", active}});
  }
  PushEvent({{"type", "find"}, {"count", count}, {"active", active}, {"final", final_update}});
}

// ----- Zoom ------------------------------------------------------------------

void BrowserWindow::ChangeZoom(int step) {
  CefRefPtr<CefBrowser> b = ActiveBrowser();
  if (!b) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = b->GetHost();
  const double level = step == 0 ? 0.0 : browsing::StepZoomLevel(host->GetZoomLevel(), step);
  host->SetZoomLevel(level);
  test_log::Event("zoom", {{"percent", browsing::ZoomPercentForLevel(host->GetZoomLevel())}});
  PushState();
}

// ----- Bookmarks and menus ---------------------------------------------------

CefPoint BrowserWindow::ShellToScreen(int x, int y) {
  CefPoint point(x, y);
  if (shell_view_) {
    shell_view_->ConvertPointToScreen(point);
  }
  return point;
}

BrowserWindow::Result BrowserWindow::StarActivePage(int x, int y) {
  browsing::Places* places = PlacesDb();
  if (!places) {
    return {false, Places() ? Places()->notice() : "Bookmarks are unavailable."};
  }
  Tab* t = ActiveTab();
  if (!t || !t->client || t->client->role() != BrowserRole::kWebTab ||
      !browsing::IsStorableUrl(t->url)) {
    return {false, "This page cannot be bookmarked."};
  }
  const std::string url = t->url;
  const std::vector<browsing::Bookmark> existing = places->BookmarksForUrl(url);
  if (existing.empty()) {
    const std::string title = t->title.empty() ? url : t->title;
    if (!places->AddBookmark(browsing::kBookmarksBarId, title, url, -1, PlacesService::NowMs())) {
      return {false, "The bookmark could not be saved."};
    }
    test_log::Event("bookmark_added", {{"via", "star"}});
    Places()->Notify(PlacesService::Change::kBookmarks);
    ShowNotice("Bookmarked in Bookmarks bar.");
    return {};
  }
  // Already bookmarked: offer to remove it or edit it, never remove silently.
  CefRefPtr<BrowserWindow> self = this;
  CefRefPtr<menus::Delegate> delegate =
      new menus::Delegate([self, url](int id, cef_event_flags_t) {
        if (id == kStarEdit) {
          self->OpenInternalPage("bookmarks");
          return;
        }
        browsing::Places* db = PlacesDb();
        if (id != kStarRemove || !db) {
          return;
        }
        for (const browsing::Bookmark& b : db->BookmarksForUrl(url)) {
          db->RemoveBookmark(b.id);
        }
        test_log::Event("bookmark_removed", {{"via", "star"}});
        Places()->Notify(PlacesService::Change::kBookmarks);
      });
  CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
  model->AddItem(kStarRemove, existing.size() > 1 ? "Remove bookmarks" : "Remove bookmark");
  model->AddItem(kStarEdit, "Edit in Bookmarks");
  const CefPoint point = ShellToScreen(x, y);
  CefRefPtr<CefWindow> window = window_;
  // Shown from a fresh task so the bridge reply is never held up by a menu.
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefWindow> w, CefRefPtr<CefMenuModel> m,
                             CefRefPtr<menus::Delegate> d, CefPoint p) {
                            menus::Show(w, m, d, p, CEF_MENU_ANCHOR_TOPRIGHT, "star");
                          },
                          window, model, delegate, point));
  return {};
}

Json BrowserWindow::ShieldJson(const Tab& t) const {
  const std::string scheme = guards::SchemeOf(t.url);
  if (!t.client || t.client->role() != BrowserRole::kWebTab ||
      (scheme != "http" && scheme != "https")) {
    return Json(nullptr);
  }
  PrivacyService* privacy = GetServices().privacy;
  const bool enabled = privacy && privacy->block_ads();
  const bool allowed = ads_allowed_->Covers(guards::HostOf(t.url));
  return {{"enabled", enabled},
          {"allowed", allowed},
          {"ads", t.client->ads_blocked()},
          {"trackers", t.client->trackers_blocked()}};
}

void BrowserWindow::OnBlockedCountChanged(int tab_id) {
  if (tab_id != active_tab_id_ || state_subscribers_.empty() || closing_window_) {
    return;
  }
  if (const Tab* t = FindTab(tab_id)) {
    PushEvent({{"type", "shield"}, {"tab", tab_id}, {"shield", ShieldJson(*t)}});
  }
}

BrowserWindow::Result BrowserWindow::ShowShieldMenu(int x, int y) {
  Tab* t = ActiveTab();
  if (!t || ShieldJson(*t).is_null()) {
    return {false, "Ads are blocked on web pages only."};
  }
  PrivacyService* privacy = GetServices().privacy;
  const bool enabled = privacy && privacy->block_ads();
  const std::string host = guards::HostOf(t->url);
  const bool allowed = ads_allowed_->Covers(host);
  const int ads = t->client->ads_blocked();
  const int trackers = t->client->trackers_blocked();
  const int tab_id = t->id;
  // Allowing ads on "www.example.com" allows them on example.com, which
  // covers www too. Blocking them again removes every entry covering the
  // page's host.
  std::string site = host;
  if (site.rfind("www.", 0) == 0 && site.find('.', 4) != std::string::npos) {
    site.erase(0, 4);
  }

  CefRefPtr<BrowserWindow> self = this;
  CefRefPtr<menus::Delegate> delegate =
      new menus::Delegate([self, site, host, tab_id](int id, cef_event_flags_t) {
        if (id == kShieldSettings) {
          self->OpenInternalPage("settings");
          return;
        }
        if (id != kShieldAllow && id != kShieldBlock) {
          return;
        }
        const bool allow = id == kShieldAllow;
        const std::string& name = allow ? site : host;
        bool ok = false;
        if (!self->private_) {
          PrivacyService* privacy = GetServices().privacy;
          ok = privacy && privacy->SetAdsAllowed(name, allow);
        } else if (allow) {
          ok = self->ads_allowed_->Covers(name) || self->ads_allowed_->Add(name);
        } else {
          self->ads_allowed_->Remove(name);
          ok = true;
        }
        test_log::Event("ads_allowed_changed", {{"site", name}, {"allowed", allow}, {"ok", ok}});
        if (!ok) {
          self->ShowNotice(allow ? "Ads could not be allowed on " + site + "."
                                 : "The change could not be saved.");
          return;
        }
        // Reload the page the menu was for, so the change shows.
        if (Tab* tab = self->FindTab(tab_id); tab && tab->client && tab->client->browser()) {
          tab->client->browser()->Reload();
        }
        self->PushState();
      });
  CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
  // The first line says what happened on this page; it is not a command.
  constexpr int kInfo = 90;
  std::string info;
  if (!enabled) {
    info = "Ad blocking is off";
  } else if (allowed) {
    info = "Ads are allowed on this site";
  } else {
    info = std::to_string(ads) + (ads == 1 ? " ad" : " ads") + " blocked on this page";
  }
  if (trackers > 0) {
    info += ", " + std::to_string(trackers) + (trackers == 1 ? " tracker" : " trackers") + " blocked";
  }
  model->AddItem(kInfo, info);
  model->SetEnabled(kInfo, false);
  model->AddSeparator();
  if (enabled) {
    const std::string when = private_ ? " until this window closes" : "";
    if (allowed) {
      model->AddItem(kShieldBlock, "Block ads on " + site + " again");
    } else {
      model->AddItem(kShieldAllow, "Allow ads on " + site + when);
    }
  }
  model->AddItem(kShieldSettings, "Ad blocking settings");
  const CefPoint point = ShellToScreen(x, y);
  CefRefPtr<CefWindow> window = window_;
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefWindow> w, CefRefPtr<CefMenuModel> m,
                             CefRefPtr<menus::Delegate> d, CefPoint p) {
                            menus::Show(w, m, d, p, CEF_MENU_ANCHOR_TOPRIGHT, "shield");
                          },
                          window, model, delegate, point));
  return {};
}

void BrowserWindow::ShowAppMenu(int x, int y) {
  using browsing::Command;
  CefRefPtr<BrowserWindow> self = this;
  CefRefPtr<menus::Delegate> delegate = new menus::Delegate([self](int id, cef_event_flags_t) {
    const int c = id - kAppMenuBase;
    if (c >= 0 && c <= static_cast<int>(Command::kLastCommand)) {
      self->HandleCommand({static_cast<Command>(c)});
    }
  });
  CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
  auto add = [&](Command c, const char* label, int key = 0, bool shift = false) {
    const int id = kAppMenuBase + static_cast<int>(c);
    model->AddItem(id, label);
#if defined(OS_WIN)
    if (key) {
      model->SetAccelerator(id, key, shift, true, false);
    }
#endif
  };
  add(Command::kNewTab, "New tab", 'T');
  add(Command::kNewWindow, "New window", 'N');
  add(Command::kNewPrivateWindow, "New private window", 'N', true);
  {
    const int tor_id = kAppMenuBase + static_cast<int>(Command::kNewTorWindow);
    model->AddItem(tor_id, "New Tor window");
#if defined(OS_WIN)
    model->SetAccelerator(tor_id, 'N', true, false, true);  // Alt+Shift+N
#endif
  }
  if (tor_) {
    add(Command::kNewTorIdentity, "New Tor identity");
  }
  model->AddSeparator();
  add(Command::kShowHistory, "History", 'H');
  add(Command::kShowBookmarks, "Bookmarks", 'O', true);
  add(Command::kShowDownloads, "Downloads", 'J');
  const int bar_id = kAppMenuBase + static_cast<int>(Command::kToggleBookmarksBar);
  model->AddCheckItem(bar_id, "Show bookmarks bar");
  model->SetChecked(bar_id, BookmarksBarVisible());
#if defined(OS_WIN)
  model->SetAccelerator(bar_id, 'B', true, true, false);
#endif
  model->AddSeparator();
  int percent = 100;
  if (CefRefPtr<CefBrowser> b = ActiveBrowser()) {
    percent = browsing::ZoomPercentForLevel(b->GetHost()->GetZoomLevel());
  }
  add(Command::kZoomOut, "Zoom out", 0xBD);
  add(Command::kZoomReset, ("Reset zoom (" + std::to_string(percent) + "%)").c_str(), '0');
  add(Command::kZoomIn, "Zoom in", 0xBB);
  model->AddSeparator();
  add(Command::kFind, "Find in page", 'F');
  model->AddSeparator();
  add(Command::kSettings, "Settings");
  const CefPoint point = ShellToScreen(x, y);
  CefRefPtr<CefWindow> window = window_;
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefWindow> w, CefRefPtr<CefMenuModel> m,
                             CefRefPtr<menus::Delegate> d, CefPoint p) {
                            menus::Show(w, m, d, p, CEF_MENU_ANCHOR_TOPRIGHT, "app");
                          },
                          window, model, delegate, point));
}

void BrowserWindow::FillBookmarkMenu(CefRefPtr<CefMenuModel> model, int64_t folder, int depth,
                                     std::vector<std::string>* urls, int* next_submenu_id) {
  browsing::Places* places = PlacesDb();
  if (!places) {
    return;
  }
  std::size_t added = 0;
  for (const browsing::Bookmark& b : places->Children(folder)) {
    if (urls->size() >= kMaxBookmarkMenuItems) {
      break;
    }
    if (b.kind == browsing::BookmarkKind::kFolder) {
      if (depth >= kMaxBookmarkMenuDepth) {
        continue;
      }
      CefRefPtr<CefMenuModel> sub =
          model->AddSubMenu((*next_submenu_id)++, MenuLabel(b.title.empty() ? "Folder" : b.title));
      FillBookmarkMenu(sub, b.id, depth + 1, urls, next_submenu_id);
    } else {
      urls->push_back(b.url);
      model->AddItem(static_cast<int>(urls->size()), MenuLabel(b.title.empty() ? b.url : b.title));
    }
    ++added;
  }
  if (added == 0) {
    model->AddItem(kBookmarkMenuEmpty, "(empty)");
    model->SetEnabledAt(model->GetCount() - 1, false);
  }
}

BrowserWindow::Result BrowserWindow::ShowBookmarkFolderMenu(int64_t folder_id, int x, int y) {
  browsing::Places* places = PlacesDb();
  if (!places) {
    return {false, "Bookmarks are unavailable."};
  }
  auto folder = places->GetBookmark(folder_id);
  if (!folder || folder->kind != browsing::BookmarkKind::kFolder) {
    return {false, "no such folder"};
  }
  auto urls = std::make_shared<std::vector<std::string>>();
  CefRefPtr<BrowserWindow> self = this;
  CefRefPtr<menus::Delegate> delegate =
      new menus::Delegate([self, urls](int id, cef_event_flags_t flags) {
        if (id < 1 || static_cast<std::size_t>(id) > urls->size()) {
          return;
        }
        const bool new_tab = (flags & EVENTFLAG_MIDDLE_MOUSE_BUTTON) != 0;
        self->OpenUrl(0, (*urls)[static_cast<std::size_t>(id - 1)], new_tab);
      });
  CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
  int next_submenu_id = kBookmarkSubmenuBase;
  FillBookmarkMenu(model, folder_id, 0, urls.get(), &next_submenu_id);
  const CefPoint point = ShellToScreen(x, y);
  CefRefPtr<CefWindow> window = window_;
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefWindow> w, CefRefPtr<CefMenuModel> m,
                             CefRefPtr<menus::Delegate> d, CefPoint p) {
                            menus::Show(w, m, d, p, CEF_MENU_ANCHOR_TOPLEFT, "bookmark_folder");
                          },
                          window, model, delegate, point));
  return {};
}

// ----- Commands --------------------------------------------------------------

void BrowserWindow::HandleCommand(browsing::ShortcutCommand command) {
  using browsing::Command;
  if (closing_window_) {
    return;
  }
  test_log::Event("command", {{"id", static_cast<int>(command.command)}});
  switch (command.command) {
    case Command::kNewTab:
      NewTab();
      break;
    case Command::kNewWindow:
      Open("", false);
      break;
    case Command::kNewPrivateWindow:
      Open("", true, this);
      break;
    case Command::kNewTorWindow:
      OpenTor("", this);
      break;
    case Command::kNewTorIdentity:
      if (GetServices().tor_new_identity) {
        GetServices().tor_new_identity(this);
      }
      break;
    case Command::kCloseTab:
      if (active_tab_id_) {
        CloseTab(active_tab_id_);
      }
      break;
    case Command::kCloseWindow:
      if (window_) {
        window_->Close();
      }
      break;
    case Command::kReopenClosedTab:
      ReopenClosedTab();
      break;
    case Command::kFocusAddress:
      if (shell_view_) {
        shell_view_->RequestFocus();
      }
      PushEvent({{"type", "focusAddress"}});
      break;
    case Command::kReload:
      Reload(false);
      break;
    case Command::kReloadBypassingCache:
      Reload(true);
      break;
    case Command::kBack:
      GoBack();
      break;
    case Command::kForward:
      GoForward();
      break;
    case Command::kHome:
      Home();
      break;
    case Command::kNextTab:
    case Command::kPreviousTab: {
      if (tabs_.size() < 2) {
        break;
      }
      const int pos = IndexOf(active_tab_id_);
      std::size_t i = pos < 0 ? 0 : static_cast<std::size_t>(pos);
      i = command.command == Command::kNextTab ? (i + 1) % tabs_.size()
                                               : (i + tabs_.size() - 1) % tabs_.size();
      Activate(tabs_[i].id);
      break;
    }
    case Command::kSelectTab:
      SelectTabNumber(command.tab_number);
      break;
    case Command::kMoveTabLeft:
      MoveActiveTab(-1);
      break;
    case Command::kMoveTabRight:
      MoveActiveTab(1);
      break;
    case Command::kFind:
      if (!ActiveTab()) {
        break;
      }
      if (!find_open_) {
        find_open_ = true;
        find_tab_id_ = active_tab_id_;
        find_text_.clear();
        RelayoutShell();
        PushState();
      }
      if (shell_view_) {
        shell_view_->RequestFocus();
      }
      PushEvent({{"type", "focusFind"}});
      break;
    case Command::kFindNext:
    case Command::kFindPrevious:
      if (find_open_ && !find_text_.empty()) {
        Find(find_text_, command.command == Command::kFindNext, find_match_case_);
      } else {
        HandleCommand({Command::kFind});
      }
      break;
    case Command::kZoomIn:
      ChangeZoom(1);
      break;
    case Command::kZoomOut:
      ChangeZoom(-1);
      break;
    case Command::kZoomReset:
      ChangeZoom(0);
      break;
    case Command::kBookmarkPage:
      // The shell answers with bookmarks.star and the star's position.
      PushEvent({{"type", "star"}});
      break;
    case Command::kToggleBookmarksBar: {
      Services& services = GetServices();
      if (services.settings) {
        services.settings->Change(kBookmarksBarSetting, Json(!BookmarksBarVisible()));
      }
      break;
    }
    case Command::kShowBookmarks:
      OpenInternalPage("bookmarks");
      break;
    case Command::kShowDownloads:
      OpenInternalPage("downloads");
      break;
    case Command::kShowHistory:
      OpenInternalPage("history");
      break;
    case Command::kSettings:
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
                               CefRefPtr<CefPanel> parent,
                               CefRefPtr<CefOverlayController> overlay) {
  if (!client) {
    return;
  }
  closing_[client.get()] = Closing{view, parent, overlay};
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
    closing_[client] = Closing{it->view, content_, nullptr};
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
                            CefRefPtr<CefOverlayController> overlay =
                                std::move(it->second.overlay);
                            it->second.view = nullptr;
                            it->second.parent = nullptr;
                            it->second.overlay = nullptr;
                            if (overlay) {
                              overlay->Destroy();  // Removes the view with it.
                            } else if (parent && view) {
                              parent->RemoveChildView(view);
                            }
                            view = nullptr;
                            parent = nullptr;
                            overlay = nullptr;
                          },
                          self, CefRefPtr<TabClient>(client)));
}

void BrowserWindow::OnClientClosed(TabClient* client) {
  closing_.erase(client);
  if (client == shell_client_.get()) {
    shell_client_ = nullptr;
    shell_view_ = nullptr;
  }
  if (client == suggest_client_.get()) {
    suggest_client_ = nullptr;
    suggest_view_ = nullptr;
    suggest_overlay_ = nullptr;
    suggest_shown_ = false;
  }
  for (std::vector<Subscriber>* list : {&state_subscribers_, &suggest_subscribers_}) {
    for (auto it = list->begin(); it != list->end();) {
      auto entry = Registry().Find(it->browser_id);
      it = entry ? std::next(it) : list->erase(it);
    }
  }
  --live_clients_;
  if (closing_window_ && live_clients_ == 0 && window_) {
    CefRefPtr<CefWindow> window = window_;
    CefPostTask(TID_UI, base::BindOnce(&CefWindow::Close, window));
  }
}

bool BrowserWindow::RecordsHistory(const Tab& tab) const {
  return !private_ && tab.client && tab.client->role() == BrowserRole::kWebTab;
}

void BrowserWindow::OnTitleChange(int tab_id, const std::string& title) {
  if (Tab* t = FindTab(tab_id)) {
    t->title = title;
    if (RecordsHistory(*t) && PlacesDb() && browsing::IsStorableUrl(t->url)) {
      PlacesDb()->SetTitle(t->url, title);
    }
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

void BrowserWindow::OnPageLoaded(int tab_id, const std::string& url) {
  Tab* t = FindTab(tab_id);
  if (!t) {
    return;
  }
  const bool record = RecordsHistory(*t) && PlacesDb() && browsing::IsStorableUrl(url);
  test_log::Event("page_loaded", {{"tab", tab_id}, {"private", private_}, {"recorded", record}});
  if (!record) {
    return;
  }
  if (PlacesDb()->AddVisit(url, t->title.empty() || t->title == url ? "" : t->title,
                           PlacesService::NowMs())) {
    Places()->Notify(PlacesService::Change::kHistory);
  }
}

void BrowserWindow::OnTorHeldBack(int tab_id) {
  if (Tab* t = FindTab(tab_id)) {
    t->loading = false;
    PushState();
  }
}

void BrowserWindow::OnTorStatus() {
  if (closing_window_) {
    return;
  }
  PushState();
  Services& services = GetServices();
  if (!services.tor || !services.tor->connected()) {
    return;
  }
  for (Tab& t : tabs_) {
    if (!t.client || !t.client->tor() || !t.client->browser()) {
      continue;
    }
    const std::string url = t.client->TakeTorPendingUrl();
    if (!url.empty()) {
      test_log::Event("tor_pending_loaded", {{"tab", t.id}});
      t.client->browser()->GetMainFrame()->LoadURL(url);
    }
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
    closed_tabs_.Clear();
    for (Tab& t : tabs) {
      StartClose(t.client, t.view, content_);
    }
    // Hand the shell's view to the close sequence and drop our own reference:
    // a browser whose view is still referenced never finishes closing
    // (docs/STAGE0_FINDINGS.md, spike B).
    CefRefPtr<CefBrowserView> shell_view = std::move(shell_view_);
    shell_view_ = nullptr;
    StartClose(shell_client_, shell_view, window_);
    // The suggestion list, if it was ever made: its overlay goes with it.
    suggestions_.clear();
    suggest_shown_ = false;
    CefRefPtr<CefBrowserView> suggest_view = std::move(suggest_view_);
    suggest_view_ = nullptr;
    CefRefPtr<CefOverlayController> suggest_overlay = std::move(suggest_overlay_);
    suggest_overlay_ = nullptr;
    StartClose(suggest_client_, suggest_view, nullptr, suggest_overlay);
  }
  return false;
}

void BrowserWindow::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  Services& services = GetServices();
  CancelPrompts(0);
  if (services.downloads) {
    if (downloads_observer_) {
      services.downloads->RemoveObserver(downloads_observer_);
      downloads_observer_ = 0;
    }
    // A private window's downloads leave the list with it; a normal
    // window's unfinished ones end with it.
    if (private_) {
      services.downloads->ForgetWindow(id_);
    } else {
      services.downloads->CancelForTab(id_, 0, "window closed");
    }
  }
  if (services.settings && settings_observer_) {
    services.settings->RemoveObserver(settings_observer_);
    settings_observer_ = 0;
  }
  if (services.tor && tor_observer_) {
    services.tor->RemoveObserver(tor_observer_);
    tor_observer_ = 0;
  }
  if (services.places && places_observer_) {
    services.places->RemoveObserver(places_observer_);
    places_observer_ = 0;
  }
  state_subscribers_.clear();
  suggest_subscribers_.clear();
  suggestions_.clear();
  suggest_view_ = nullptr;
  suggest_client_ = nullptr;
  suggest_overlay_ = nullptr;
  closing_.clear();
  tabs_.clear();
  closed_tabs_.Clear();
  content_ = nullptr;
  shell_view_ = nullptr;
  shell_client_ = nullptr;
  window_ = nullptr;
  // A private window's context (its whole off-the-record profile) goes with
  // the window.
  web_context_ = nullptr;
  test_log::Event("window_destroyed", {{"private", private_}, {"tor", tor_}, {"id", id_}});
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
  const std::string name = tor_ ? "TorFox Tor" : private_ ? "TorFox Private" : "TorFox";
  std::string title = t && !t->title.empty() ? t->title + " \xE2\x80\x94 " + name : name;
  if (kTestHooks) {
    title += " (TEST BUILD)";  // Test-hook builds must never pass for a release.
  }
  window_->SetTitle(title);
}

Json BrowserWindow::DownloadsJson() const {
  DownloadService* d = GetServices().downloads;
  if (!d) {
    return {{"active", 0}, {"count", 0}};
  }
  const DownloadService::Viewer viewer{private_, id_};
  return {{"active", d->ActiveCount(viewer)}, {"count", d->VisibleCount(viewer)}};
}

Json BrowserWindow::StateJson() {
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
      const bool web = t.client && t.client->role() == BrowserRole::kWebTab;
      const bool bookmarkable = web && browsing::IsStorableUrl(t.url);
      bool bookmarked = false;
      if (bookmarkable && PlacesDb()) {
        bookmarked = !PlacesDb()->BookmarksForUrl(t.url).empty();
      }
      int zoom = 100;
      if (t.client && t.client->browser()) {
        zoom = browsing::ZoomPercentForLevel(t.client->browser()->GetHost()->GetZoomLevel());
      }
      nav = {{"url", t.url},
             {"internal", internal},
             {"loading", t.loading},
             {"canGoBack", t.can_back},
             {"canGoForward", t.can_forward},
             {"bookmarkable", bookmarkable},
             {"bookmarked", bookmarked},
             {"zoom", zoom},
             {"shield", ShieldJson(t)}};
    }
  }
  Json update = UpdateJson();
  update_ready_shown_ = !update.is_null();
  return {{"type", "state"},
          {"private", private_},
          {"tor", tor_ ? TorJson() : Json(nullptr)},
          {"tabs", tabs},
          {"activeId", active_tab_id_},
          {"nav", nav},
          {"findOpen", find_open_},
          {"bookmarksBar", BookmarksBarVisible()},
          {"downloads", DownloadsJson()},
          {"update", update}};
}

Json BrowserWindow::UpdateJson() {
  UpdateService* updates = GetServices().updates;
  if (!updates) {
    return Json(nullptr);
  }
  const UpdateService::Status s = updates->status();
  if (s.state != UpdateService::State::kReady) {
    return Json(nullptr);
  }
  // Windows: "Relaunch to update" installs and starts TorFox again. macOS:
  // TorFox closes and opens the disk image.
  return {{"version", s.version}, {"restarts", platform::InstallsUpdatesOnExit()}};
}

void BrowserWindow::CollectRenderers(std::vector<memory::Renderer>* out) const {
  const std::string kind = tor_ ? "tor" : private_ ? "private" : "normal";
  for (const Tab& t : tabs_) {
    if (!t.client) {
      continue;
    }
    for (long pid : t.client->renderer_pids()) {
      memory::Renderer r;
      r.pid = pid;
      r.tab = true;
      r.tab_key = static_cast<int64_t>(id_) * 1000000 + t.id;
      r.title = t.title.empty() ? t.url : t.title;
      r.window = kind;
      r.active = t.id == active_tab_id_;
      out->push_back(std::move(r));
    }
  }
  for (const CefRefPtr<TabClient>& own : {shell_client_, suggest_client_}) {
    if (!own) {
      continue;
    }
    for (long pid : own->renderer_pids()) {
      memory::Renderer r;
      r.pid = pid;
      r.window = kind;
      out->push_back(std::move(r));
    }
  }
}

void BrowserWindow::UpdateStatusChanged() {
  // Only when the button appears or goes: not for each percent downloaded.
  const bool ready = !UpdateJson().is_null();
  if (ready == update_ready_shown_ || state_subscribers_.empty()) {
    return;
  }
  PushEvent(StateJson());  // Remembers what it sent.
}

Json BrowserWindow::TorJson() const {
  Services& services = GetServices();
  if (!services.tor) {
    return {{"state", "error"}, {"progress", 0}, {"message", "Tor is not available."},
            {"retry", false}, {"connected", false}};
  }
  const TorService::Status s = services.tor->status();
  return {{"state", TorService::StateName(s.state)},
          {"progress", s.progress},
          {"message", s.message.substr(0, 300)},
          {"retry", s.retry},
          {"connected", services.tor->connected()}};
}

Json BrowserWindow::BookmarksBarJson() {
  Json items = Json::array();
  const bool visible = BookmarksBarVisible();
  if (visible && PlacesDb()) {
    for (const browsing::Bookmark& b : PlacesDb()->Children(browsing::kBookmarksBarId)) {
      if (items.size() >= kMaxBookmarksBarItems) {
        break;
      }
      items.push_back({{"id", b.id},
                       {"title", b.title},
                       {"url", b.url},
                       {"folder", b.kind == browsing::BookmarkKind::kFolder}});
    }
  }
  return {{"type", "bookmarksBar"}, {"visible", visible}, {"items", items}};
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
  // Every change to the tabs comes through here.
  if (!private_) {
    if (Services& services = GetServices(); services.session_changed) {
      services.session_changed();
    }
  }
}

void BrowserWindow::PushBookmarksBar() {
  if (!state_subscribers_.empty()) {
    PushEvent(BookmarksBarJson());
  }
}

void BrowserWindow::ShowNotice(const std::string& text) {
  PushEvent({{"type", "notice"}, {"text", text}});
}

void BrowserWindow::ConfirmDownload(int tab_id, const platform::ConfirmPrompt& prompt,
                                    std::function<void(bool)> done) {
  ConfirmForTab(tab_id, "download", prompt, std::move(done));
}

std::shared_ptr<platform::PromptHandle> BrowserWindow::ConfirmForTab(
    int tab_id,
    const char* kind,
    const platform::ConfirmPrompt& prompt,
    std::function<void(bool)> done) {
  CEF_REQUIRE_UI_THREAD();
  if (!window_ || closing_window_ || !FindTab(tab_id)) {
    CefPostTask(TID_UI, base::BindOnce([](std::function<void(bool)> f) { f(false); },
                                       std::move(done)));
    return nullptr;
  }
  // The prompt belongs to its tab: show that tab, so the user sees which
  // page asked.
  if (active_tab_id_ != tab_id) {
    Activate(tab_id);
  }
  auto slot = std::make_shared<std::shared_ptr<platform::PromptHandle>>();
  CefRefPtr<BrowserWindow> self = this;
  std::shared_ptr<platform::PromptHandle> handle = dialogs::Confirm(
      window_, kind, prompt,
      [self, slot, done = std::move(done)](bool ok) {
        auto& prompts = self->prompts_;
        prompts.erase(std::remove_if(prompts.begin(), prompts.end(),
                                     [&](const Prompt& p) { return p.handle == *slot; }),
                      prompts.end());
        done(ok);
      });
  *slot = handle;
  if (handle) {
    prompts_.push_back(Prompt{tab_id, kind, handle});
  }
  return handle;
}

void BrowserWindow::CancelPrompts(int tab_id, const char* kind) {
  // Cancel() answers asynchronously, and the answer removes the entry.
  std::vector<std::shared_ptr<platform::PromptHandle>> cancel;
  for (const Prompt& p : prompts_) {
    if ((tab_id == 0 || p.tab_id == tab_id) && (!kind || std::string_view(kind) == p.kind)) {
      cancel.push_back(p.handle);
    }
  }
  for (const auto& handle : cancel) {
    handle->Cancel();
  }
}

void BrowserWindow::AddStateSubscriber(int browser_id,
                                       int64_t query_id,
                                       CefRefPtr<CefMessageRouterBrowserSide::Callback> callback) {
  state_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
  callback->Success(DumpJson(StateJson()));
  callback->Success(DumpJson(BookmarksBarJson()));
  std::string& notice = GetServices().startup_notice;
  if (!notice.empty()) {
    Json event = Json::object();
    event["type"] = "notice";
    event["text"] = notice;
    callback->Success(DumpJson(event));
    notice.clear();
  }
}

void BrowserWindow::RemoveStateSubscriber(int browser_id, int64_t query_id) {
  for (std::vector<Subscriber>* list : {&state_subscribers_, &suggest_subscribers_}) {
    list->erase(std::remove_if(list->begin(), list->end(),
                               [&](const Subscriber& s) {
                                 return s.browser_id == browser_id && s.query_id == query_id;
                               }),
                list->end());
  }
}

// ----- Address-bar suggestions -------------------------------------------------

Json BrowserWindow::SuggestionsJson() const {
  Json items = Json::array();
  for (const browsing::Suggestion& s : suggestions_) {
    items.push_back({{"url", s.url},
                     {"title", s.title},
                     {"source", s.source == browsing::Suggestion::Source::kBookmark ? "bookmark"
                                                                                    : "history"}});
  }
  return {{"items", items}, {"highlight", suggest_highlight_}};
}

void BrowserWindow::PushSuggestions() {
  const std::string payload = DumpJson(SuggestionsJson());
  for (Subscriber& s : suggest_subscribers_) {
    s.callback->Success(payload);
  }
}

Json BrowserWindow::SuggestQuery(const std::string& text, int x, int y, int width) {
  CEF_REQUIRE_UI_THREAD();
  suggestions_.clear();
  suggest_highlight_ = -1;
  browsing::Places* db = PlacesDb();
  if (db && window_ && shell_view_ && !closing_window_) {
    // Tor windows: bookmarks only, so nothing of normal browsing shows up in
    // them. Nothing is ever looked up on the network.
    suggestions_ = db->Suggest(text, static_cast<std::size_t>(bridge::kMaxSuggestions), !tor_);
  }
  if (suggestions_.empty()) {
    SuggestHide();
    return SuggestionsJson();
  }
  if (!suggest_view_) {
    CefRefPtr<TabClient> client;
    suggest_view_ = CreateView(BrowserRole::kSuggest, 0, InternalPageUrl("suggest"), &client);
    suggest_client_ = client;
    // Never activated: the address bar keeps the focus while the list shows.
    suggest_overlay_ = window_->AddOverlayView(suggest_view_, CEF_DOCKING_MODE_CUSTOM, false);
  }
  CefPoint origin(x, y);
  shell_view_->ConvertPointToWindow(origin);
  const int height =
      kSuggestFrameHeight + kSuggestRowHeight * static_cast<int>(suggestions_.size());
  if (suggest_overlay_) {
    suggest_overlay_->SetBounds(CefRect(origin.x, origin.y, width, height));
    suggest_overlay_->SetVisible(true);
  }
  suggest_shown_ = true;
  PushSuggestions();
  test_log::Event("suggest_shown", {{"count", suggestions_.size()},
                                    {"bounds", Json::array({origin.x, origin.y, width, height})}});
  return SuggestionsJson();
}

void BrowserWindow::SuggestHighlight(int index) {
  if (index < -1 || index >= static_cast<int>(suggestions_.size())) {
    return;
  }
  suggest_highlight_ = index;
  PushSuggestions();
}

void BrowserWindow::SuggestHide() {
  const bool was_shown = suggest_shown_;
  suggest_shown_ = false;
  suggestions_.clear();
  suggest_highlight_ = -1;
  if (suggest_overlay_ && was_shown) {
    suggest_overlay_->SetVisible(false);
  }
  if (was_shown) {
    PushSuggestions();
    test_log::Event("suggest_hidden", Json::object());
  }
}

BrowserWindow::Result BrowserWindow::SuggestOpen(int index) {
  if (!suggest_shown_ || index < 0 || index >= static_cast<int>(suggestions_.size())) {
    return {false, "no such suggestion"};
  }
  const std::string url = suggestions_[static_cast<std::size_t>(index)].url;
  SuggestHide();
  Result r = OpenUrl(0, url, false);
  PushEvent({{"type", "suggestOpened"}, {"ok", r.ok}});
  test_log::Event("suggest_opened", {{"ok", r.ok}});
  return r;
}

void BrowserWindow::AddSuggestSubscriber(
    int browser_id,
    int64_t query_id,
    CefRefPtr<CefMessageRouterBrowserSide::Callback> callback) {
  suggest_subscribers_.push_back(Subscriber{browser_id, query_id, callback});
  callback->Success(DumpJson(SuggestionsJson()));
}

}  // namespace torfox
