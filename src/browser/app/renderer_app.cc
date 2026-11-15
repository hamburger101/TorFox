// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/renderer_app.h"

#include "src/browser/app/app_common.h"
#include "src/core/url/internal_url.h"

namespace torfox {

namespace {

// WebRTC looks up STUN and TURN servers and remote candidates by name
// itself, on this computer's resolver, not through the proxy: in a Tor
// window that is a DNS leak (docs/STAGE9_FINDINGS.md). Tor windows get no
// WebRTC at all, as in Tor Browser. RTCPeerConnection is exposed to windows
// only, and every frame's context (about:blank and srcdoc frames included)
// passes through OnContextCreated before any of its scripts run.
constexpr const char* kWebRtcInterfaces[] = {
    "RTCPeerConnection", "webkitRTCPeerConnection", "RTCDataChannel",
    "RTCRtpTransceiver", "RTCRtpSender",            "RTCRtpReceiver",
    "RTCIceTransport",   "RTCDtlsTransport",        "RTCSctpTransport",
};

void RemoveWebRtc(CefRefPtr<CefV8Context> context) {
  if (!context || !context->Enter()) {
    return;
  }
  CefRefPtr<CefV8Value> global = context->GetGlobal();
  for (const char* name : kWebRtcInterfaces) {
    // Unconditionally: some interfaces are installed lazily and are not
    // reported by HasValue until first used.
    global->DeleteValue(name);
    if (global->HasValue(name)) {
      // Not deletable: shadow it with a read-only undefined.
      global->SetValue(name, CefV8Value::CreateUndefined(),
                       static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY |
                                                               V8_PROPERTY_ATTRIBUTE_DONTDELETE));
    }
  }
  context->Exit();
}

}  // namespace

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
  if (extra_info && extra_info->HasKey(kExtraInfoTor) && extra_info->GetBool(kExtraInfoTor)) {
    tor_browsers_.insert(browser->GetIdentifier());
  }
}

void RendererApp::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) {
  internal_browsers_.erase(browser->GetIdentifier());
  tor_browsers_.erase(browser->GetIdentifier());
}

void RendererApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  if (tor_browsers_.count(browser->GetIdentifier()) != 0) {
    RemoveWebRtc(context);
  }
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
