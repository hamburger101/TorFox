// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_APP_COMMON_H_
#define TORFOX_BROWSER_APP_APP_COMMON_H_

#include "include/cef_scheme.h"
#include "include/wrapper/cef_message_router.h"

namespace torfox {

// Must be identical in every process type (CEF requirement).
void RegisterTorFoxSchemes(CefRawPtr<CefSchemeRegistrar> registrar);

// Shared by the browser- and renderer-side message routers. The JS names are
// only ever injected into TorFox's own internal pages.
CefMessageRouterConfig BridgeRouterConfig();

// Key in the browser's extra_info marking it as internal (set only by the
// browser process when it creates shell/internal-tab browsers).
inline constexpr char kExtraInfoInternal[] = "torfox.internal";
// Web tabs of Tor windows: their renderers remove WebRTC (RendererApp).
inline constexpr char kExtraInfoTor[] = "torfox.tor";
// Renderer -> browser, as each frame's script context is created: the
// renderer's process ID, for Settings > Memory. Any renderer can send it, so
// the browser only uses it to attribute a process it has itself found to be
// one of TorFox's helpers (platform::TorFoxProcesses).
inline constexpr char kRendererPidMessage[] = "torfox_renderer_pid";

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_APP_COMMON_H_
