// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_ADBLOCK_SITES_H_
#define TORFOX_CORE_ADBLOCK_SITES_H_

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace torfox::adblock {

// The sites the user allowed ads on. In normal windows they are the setting
// "privacy.adsAllowedSites": host names, one per line; a name covers its
// subdomains too. Private and Tor windows keep their own list in memory.

// The setting's check: every non-empty line is a host name (letters, digits,
// hyphens and dots; an IPv4 address is one too), at most 1000 of them.
bool SitesTextIsValid(std::string_view text);

// The host names in |text| (lowercase, without a trailing dot), each once,
// in their order. Lines that are not host names are skipped.
std::vector<std::string> ParseSites(std::string_view text);

// |sites| as the setting stores them.
std::string SitesText(const std::vector<std::string>& sites);

// The entry of |sites| that covers |host| (|host| itself or a parent
// domain), or "" if none does.
std::string_view CoveringSite(const std::vector<std::string>& sites, std::string_view host);

// A list shared between threads (the request handlers read it on the
// network thread).
class SiteList {
 public:
  void Set(std::vector<std::string> sites);
  std::vector<std::string> Get() const;
  bool Covers(std::string_view host) const;
  // Adds |host| (lowercase). False if it was covered already.
  bool Add(std::string_view host);
  // Removes every entry covering |host|. False if there was none.
  bool Remove(std::string_view host);

 private:
  mutable std::mutex lock_;
  std::vector<std::string> sites_;
};

}  // namespace torfox::adblock

#endif  // TORFOX_CORE_ADBLOCK_SITES_H_
