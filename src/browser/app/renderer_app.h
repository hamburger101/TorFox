// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_RENDERER_APP_H_
#define TORFOX_BROWSER_APP_RENDERER_APP_H_

#include <set>

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

namespace torfox {

// CefApp for sandboxed renderer processes. Its only job is to expose the
// bridge functions (window.torfoxQuery / torfoxQueryCancel) to TorFox's own
// internal pages and to nothing else:
//  * the browser must have been created by the browser process with the
//    internal flag in extra_info, and
//  * the frame must be a main frame whose URL is a torfox:// page.
// This is a convenience boundary only: a compromised renderer could forge
// messages, which is why the browser process re-validates every message.
class RendererApp : public CefApp, public CefRenderProcessHandler {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

  void OnWebKitInitialized() override;
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue> extra_info) override;
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

 private:
  CefRefPtr<CefMessageRouterRendererSide> router_;
  std::set<int> internal_browsers_;
  IMPLEMENT_REFCOUNTING(RendererApp);
};

// CefApp for GPU, utility and other helper processes.
class OtherApp : public CefApp {
 public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

 private:
  IMPLEMENT_REFCOUNTING(OtherApp);
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_RENDERER_APP_H_
