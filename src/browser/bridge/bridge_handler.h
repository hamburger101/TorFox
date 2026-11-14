// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_BRIDGE_BRIDGE_HANDLER_H_
#define TORFOX_BROWSER_BRIDGE_BRIDGE_HANDLER_H_

#include <vector>

#include "include/wrapper/cef_message_router.h"
#include "src/core/bridge/validator.h"

namespace torfox {

// Browser-side end of the UI bridge. Every query is validated by
// bridge::Validate() using the browser process's own records (registry role,
// main frame, committed URL) before anything is executed. Rejections return a
// generic failure to the page and are logged for tests.
class BridgeHandler : public CefMessageRouterBrowserSide::Handler {
 public:
  BridgeHandler();
  ~BridgeHandler() override;

  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override;
  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               CefRefPtr<const CefBinaryBuffer> request,
               bool persistent,
               CefRefPtr<Callback> callback) override;
  void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int64_t query_id) override;

  // Releases callbacks; call before CefShutdown().
  void Shutdown();

 private:
  void Dispatch(const bridge::Validated& msg,
                CefRefPtr<CefBrowser> browser,
                int64_t query_id,
                CefRefPtr<Callback> callback);
  void PushAppearance();

  struct Subscriber {
    int browser_id;
    int64_t query_id;
    CefRefPtr<Callback> callback;
  };
  std::vector<Subscriber> appearance_subscribers_;
  int settings_observer_ = 0;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_BRIDGE_BRIDGE_HANDLER_H_
