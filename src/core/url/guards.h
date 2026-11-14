// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_URL_GUARDS_H_
#define TORFOX_CORE_URL_GUARDS_H_

#include <string>
#include <string_view>

// Pure decision functions behind TorFox's request guards. The browser layer
// calls them from CEF's OnBeforeBrowse / OnBeforeResourceLoad / scheme
// handler; keeping them here lets unit tests cover every case directly,
// independent of the checks Chromium itself applies first.
namespace torfox::guards {

// What kind of browser a request comes from.
enum class Role {
  kShell,        // The window's tab strip and toolbar (torfox://shell/ only).
  kInternalTab,  // A tab showing a TorFox page (newtab, settings).
  kWebTab,       // A tab showing web content.
};

bool IsInternal(Role role);

// Lower-case scheme of |url| ("" if none).
std::string SchemeOf(std::string_view url);

// Lower-case host of a hierarchical URL ("scheme://[userinfo@]host[:port]...").
// IPv6 literals are returned without brackets. "" if there is no authority.
std::string HostOf(std::string_view url);

// "x.onion" and "a.b.onion." are onion hosts; "onion" and "onion.com" are not.
bool IsOnionHost(std::string_view host);

// Schemes web content must never reach, whatever Chromium's own checks say.
bool IsPrivilegedScheme(std::string_view scheme);

// Why a navigation (main frame or subframe) must be cancelled; "" to allow.
//   Internal browsers: only known torfox:// pages, and only the page that
//   matches the role (the shell shows torfox://shell/, tabs never do).
//   Web tabs: http(s), data:, blob:, about:blank and about:srcdoc only; never
//   a privileged scheme, never an onion host (until Tor tabs exist).
std::string NavigationBlockReason(Role role, std::string_view url);

// Why a subresource request must be cancelled; "" to allow.
// Note: CEF never passes WebSocket handshakes to OnBeforeResourceLoad, so
// ws:/wss: verdicts here are not enforced by the browser today; onion
// WebSockets are stopped by the forced host-resolver rule instead
// (tests/e2e/test_quiet_startup.py).
//   Internal browsers: their own torfox:// resources, data: and blob: only.
//   Web tabs: never a privileged scheme, never an onion host.
std::string ResourceBlockReason(Role role, std::string_view url);

// The torfox:// scheme handler's decision for one request.
struct InternalRequest {
  int status = 0;             // 200, 403, 404 or 405.
  std::string why;            // For the test log.
  std::string resource_path;  // Path in the bundled resource table when status is 200.
};

// |known| is false when the request has no browser or the browser is not in
// TorFox's registry. Only internal browsers are ever served, only with GET,
// and the shell page only to the shell (and only the shell gets it).
InternalRequest DecideInternalRequest(bool known, Role role, std::string_view method,
                                      std::string_view url);

}  // namespace torfox::guards

#endif  // TORFOX_CORE_URL_GUARDS_H_
