// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/privacy/trackers.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>

#include "src/core/url/public_suffix.h"

namespace torfox::trackers {

namespace {

#include "trackers_data.inc"  // kTrackerListCommit, kTrackerDomains (sorted).

std::string Normalize(std::string_view host) {
  std::string out(host);
  while (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

bool IsIpLiteral(std::string_view host) {
  if (host.find(':') != std::string_view::npos || (!host.empty() && host.front() == '[')) {
    return true;  // IPv6.
  }
  return !host.empty() && host.find_first_not_of("0123456789.") == std::string_view::npos;
}

}  // namespace

std::string_view ListCommit() {
  return kTrackerListCommit;
}

std::size_t ListSize() {
  return std::size(kTrackerDomains);
}

std::string_view MatchListed(std::string_view raw_host) {
  const std::string host = Normalize(raw_host);
  if (host.empty() || IsIpLiteral(host)) {
    return {};
  }
  // The host itself, then each parent domain with at least two labels.
  std::string_view candidate = host;
  while (candidate.find('.') != std::string_view::npos) {
    const auto it =
        std::lower_bound(std::begin(kTrackerDomains), std::end(kTrackerDomains), candidate);
    if (it != std::end(kTrackerDomains) && *it == candidate) {
      return *it;
    }
    candidate.remove_prefix(candidate.find('.') + 1);
  }
  return {};
}

std::string SiteOf(std::string_view raw_host) {
  const std::string host = Normalize(raw_host);
  if (host.empty() || IsIpLiteral(host)) {
    return host;
  }
  std::optional<std::string> site = psl::RegistrableDomain(host);
  return site ? *site : host;
}

Verdict Check(std::string_view request_host, std::string_view top_host) {
  Verdict v;
  v.listed = MatchListed(request_host);
  if (v.listed.empty()) {
    return v;
  }
  v.block = top_host.empty() || SiteOf(request_host) != SiteOf(top_host);
  return v;
}

}  // namespace torfox::trackers
