// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_TOR_ONION_H_
#define TORFOX_CORE_TOR_ONION_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace torfox::tor {

// DuckDuckGo's onion service (its v3 address checksum is verified by
// tor_test.cc). Searches from Tor windows go here instead of duckduckgo.com.
inline constexpr std::string_view kDuckDuckGoOnion =
    "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";

// SHA3-256 (FIPS 202). Used only for the onion address checksum, which
// catches typing mistakes before tor sees the address; tor checks it too.
std::array<uint8_t, 32> Sha3_256(std::string_view data);

enum class OnionCheck {
  kValid,
  kNotOnion,       // The host does not end in ".onion".
  kOldVersion,     // A 16-character v2 address: tor removed support for them.
  kBadLength,      // Not 56 characters.
  kBadCharacters,  // Not base32 (a-z, 2-7).
  kBadVersion,     // Decodes, but is not a version 3 address.
  kBadChecksum,    // A typing mistake: the checksum does not match.
};

// |host| as guards::HostOf returns it. Subdomains ("www.<address>.onion")
// and one trailing dot are allowed.
OnionCheck CheckOnionHost(std::string_view host);

// A sentence for the user, for every result except kValid.
std::string_view OnionCheckMessage(OnionCheck check);

struct OnionInput {
  std::string url;    // Empty if |error| is set.
  std::string error;
};

// What the user typed (the address bar's kOnion input) as the URL a Tor tab
// loads: "x.onion/path" becomes "http://x.onion/path" (onion services
// encrypt the connection themselves); an explicit http:// or https:// is
// kept. Anything else mentioning an onion host is refused, never searched.
OnionInput OnionUrlFromInput(std::string_view input);

// "https://duckduckgo.com/<rest>" as the same URL on DuckDuckGo's onion
// service; any other URL is returned unchanged.
std::string TorSearchUrl(std::string_view url);

}  // namespace torfox::tor

#endif  // TORFOX_CORE_TOR_ONION_H_
