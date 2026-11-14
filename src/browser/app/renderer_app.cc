// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/renderer_app.h"

#include "src/browser/app/app_common.h"
#include "src/core/url/internal_url.h"

namespace torfox {

void RendererApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterTorFoxSchemes(registrar);
}

void RendererApp::OnWebKitInitialized() {
  router_ = CefMessageRouterRendererSide::Create(BridgeRouterConfig());
}

void RendererApp::OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefDictionaryValue> extra_info) {
  if (extra_info && extra_info->HasKey(kExtraInfoInternal) &&
      extra_info->GetBool(kExtraInfoInternal)) {
    internal_browsers_.insert(browser->GetIdentifier());
  }
}

void RendererApp::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) {
  internal_browsers_.erase(browser->GetIdentifier());
}

void RendererApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  if (!router_ || internal_browsers_.count(browser->GetIdentifier()) == 0 || !frame->IsMain()) {
    return;
  }
  auto url = ParseInternalUrl(frame->GetURL().ToString());
  if (!url || !IsInternalPage(url->host)) {
    return;
  }
  router_->OnContextCreated(browser, frame, context);
}

void RendererApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefV8Context> context) {
  if (router_) {
    router_->OnContextReleased(browser, frame, context);
  }
}

bool RendererApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                           CefRefPtr<CefFrame> frame,
                                           CefProcessId source_process,
                                           CefRefPtr<CefProcessMessage> message) {
  if (!router_ || internal_browsers_.count(browser->GetIdentifier()) == 0) {
    return false;
  }
  return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

void OtherApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterTorFoxSchemes(registrar);
}

}  // namespace torfox
