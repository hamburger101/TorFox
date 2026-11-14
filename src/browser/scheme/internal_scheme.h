// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SCHEME_INTERNAL_SCHEME_H_
#define TORFOX_BROWSER_SCHEME_INTERNAL_SCHEME_H_

#include <string_view>

#include "include/cef_scheme.h"

namespace torfox {

// Content-Security-Policy sent with every internal page.
inline constexpr std::string_view kInternalCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
    "font-src 'self'; connect-src 'none'; media-src 'none'; object-src 'none'; "
    "frame-src 'none'; worker-src 'none'; manifest-src 'none'; frame-ancestors 'none'; "
    "base-uri 'none'; form-action 'none'; require-trusted-types-for 'script'; "
    "trusted-types lit-html";

// Factory for torfox:// requests. Registered on the Internal request context
// only. Serves exclusively from the compiled-in resource table, and only to
// browsers the browser registry lists as internal.
CefRefPtr<CefSchemeHandlerFactory> CreateInternalSchemeFactory();

}  // namespace torfox

#endif  // TORFOX_BROWSER_SCHEME_INTERNAL_SCHEME_H_
