// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_PRIVACY_TRACKERS_H_
#define TORFOX_CORE_PRIVACY_TRACKERS_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace torfox::trackers {

// TorFox's tracker list: the whole-domain rules of EasyPrivacy, compiled in
// from third_party/easyprivacy (scripts/gen_trackers.py).
std::string_view ListCommit();
std::size_t ListSize();

// The listed domain |host| is, or is a subdomain of ("" if none). |host| is
// a URL host as Chromium reports it (ASCII; any case; a trailing dot is
// ignored). IP literals never match.
std::string_view MatchListed(std::string_view host);

// The site of |host| for third-party checks: its registrable domain
// (eTLD+1, Public Suffix List with private entries), or the host itself
// when it has none (IP addresses, "localhost", a bare public suffix).
std::string SiteOf(std::string_view host);

struct Verdict {
  bool block = false;
  std::string_view listed;  // The list entry that matched.
};

// Whether a request to |request_host|, made by a page whose top-level host is
// |top_host|, is a tracker request to block: the request host is listed and
// belongs to another site than the page. A page's own site is never blocked,
// so visiting a listed domain directly still works. An empty |top_host|
// (no page yet) counts as another site.
Verdict Check(std::string_view request_host, std::string_view top_host);

}  // namespace torfox::trackers

#endif  // TORFOX_CORE_PRIVACY_TRACKERS_H_
