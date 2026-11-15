// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_URL_NAV_INPUT_H_
#define TORFOX_CORE_URL_NAV_INPUT_H_

#include <string>
#include <string_view>

namespace torfox {

enum class NavKind {
  kWebUrl,    // Load |url| in a web context.
  kSearch,    // Load |url|, a DuckDuckGo search URL.
  kInternal,  // Load |url|, a known torfox:// page, in the internal context.
  kOnion,     // Input names a .onion host: must go to a Tor tab, never searched.
  kRejected,  // Refused (|reason| says why).
};

struct NavDecision {
  NavKind kind = NavKind::kRejected;
  std::string url;
  std::string reason;
};

inline constexpr std::size_t kMaxNavInputLength = 8192;

// Classifies what the user typed in the address bar.
//
// Rules, in order:
//  1. Empty or over-long input, and input with control characters, is refused.
//  2. Input mentioning a .onion host anywhere is kOnion and is never searched.
//  3. "torfox://<known page>" is kInternal. Other explicit schemes are allowed
//     only for http and https; javascript:, data:, file: and others are refused.
//  4. Input without spaces that looks like a host becomes a URL: a name whose
//     last label is a top-level domain on the Public Suffix List
//     ("example.com"), a dotted name with a port or a path ("nas.lan:8080",
//     "nas.lan/"), a single label with a port ("intranet:8080"),
//     "localhost", or an IPv4 or bracketed IPv6 address.
//     Local hosts (localhost, loopback, private and link-local addresses) get
//     http://, everything else https://.
//  5. Everything else is a DuckDuckGo search.
NavDecision ClassifyNavInput(std::string_view input);

// "https://duckduckgo.com/?q=<percent-encoded query>".
std::string DuckDuckGoSearchUrl(std::string_view query);

// True if |text| contains a token whose host part ends in ".onion".
bool MentionsOnionHost(std::string_view text);

}  // namespace torfox

#endif  // TORFOX_CORE_URL_NAV_INPUT_H_
