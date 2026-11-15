// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/app_common.h"

#include <string>

#include "src/core/url/internal_url.h"

namespace torfox {

void RegisterTorFoxSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  // STANDARD: real origins (torfox://settings) and canonical URLs.
  // SECURE: treated like https (secure context, no mixed-content warnings).
  // DISPLAY_ISOLATED: content of other schemes cannot link to, frame or
  //   navigate to torfox:// URLs.
  // Deliberately NOT set: CSP_BYPASSING, FETCH_ENABLED, LOCAL.
  // CORS_ENABLED is required for same-origin ES module scripts on custom
  // schemes; no response carries Access-Control-Allow-Origin, so other
  // origins still cannot read internal resources, and only the Internal
  // request context has a handler for the scheme at all.
  registrar->AddCustomScheme(std::string(kInternalScheme),
                             CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                 CEF_SCHEME_OPTION_DISPLAY_ISOLATED |
                                 CEF_SCHEME_OPTION_CORS_ENABLED);
}

CefMessageRouterConfig BridgeRouterConfig() {
  CefMessageRouterConfig config;
  config.js_query_function = "torfoxQuery";
  config.js_cancel_function = "torfoxQueryCancel";
  return config;
}

}  // namespace torfox
