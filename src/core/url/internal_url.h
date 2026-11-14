// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_URL_INTERNAL_URL_H_
#define TORFOX_CORE_URL_INTERNAL_URL_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace torfox {

inline constexpr std::string_view kInternalScheme = "torfox";

// Internal pages served from the bundled resource table.
//   shell    - the window's tab strip and toolbar
//   newtab   - the new tab page
//   settings - the settings page
std::span<const std::string_view> InternalPages();
bool IsInternalPage(std::string_view host);

struct InternalUrl {
  std::string host;  // e.g. "settings"
  std::string path;  // always starts with '/'; excludes query and fragment
};

// Strictly parses a canonical torfox:// URL ("torfox://<host>[/<path>][?..][#..]").
// The host must be 1-32 lowercase ASCII letters. Userinfo, ports, percent
// escapes or backslashes in the authority, and upper-case schemes are
// rejected (Chromium canonicalises standard-scheme URLs to lower case before
// TorFox sees them, so anything else is not a real internal URL).
std::optional<InternalUrl> ParseInternalUrl(std::string_view url);

// Returns "torfox://<host>/" for a known page.
std::string InternalPageUrl(std::string_view host);

// Maps an internal URL to a path in the bundled resource table:
//   torfox://<page>/ and torfox://<page>/index.html -> "pages/<page>/index.html"
//   torfox://<page>/assets/<file>                  -> "assets/<file>"
// Anything else (unknown page, other paths, "..", "//", "%") has no resource.
std::optional<std::string> ResourcePathFor(const InternalUrl& url);

// True if |url| uses the torfox scheme at all (case-insensitive), whether or
// not it is well formed. Used to block any attempt to reach internal pages.
bool HasInternalScheme(std::string_view url);

}  // namespace torfox

#endif  // TORFOX_CORE_URL_INTERNAL_URL_H_
