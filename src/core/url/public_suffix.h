// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_URL_PUBLIC_SUFFIX_H_
#define TORFOX_CORE_URL_PUBLIC_SUFFIX_H_

#include <optional>
#include <string>
#include <string_view>

namespace torfox::psl {

// Public Suffix List lookups (https://publicsuffix.org), using the list
// vendored in third_party/publicsuffix and compiled in at build time.
//
// Hosts must be in ASCII form (IDN labels as punycode, as Chromium reports
// them); ASCII letters may be in any case. A host with an empty label
// (leading, trailing or doubled dot) has no registrable domain.

// Which parts of the list apply. Browsers use both for site boundaries.
enum class Sections { kIcannOnly, kIcannAndPrivate };

// The public suffix of |host| (e.g. "co.uk" for "www.example.co.uk"). An
// unlisted TLD is its own public suffix (the list's "*" default rule).
std::optional<std::string> PublicSuffix(std::string_view host,
                                        Sections sections = Sections::kIcannAndPrivate);

// The registrable domain (eTLD+1) of |host|, e.g. "example.co.uk"; nullopt
// if |host| is itself a public suffix or is malformed.
std::optional<std::string> RegistrableDomain(std::string_view host,
                                             Sections sections = Sections::kIcannAndPrivate);

// True if |label| (one label, Unicode or ASCII, any case) is a top-level
// domain in the ICANN section of the list.
bool IsKnownTld(std::string_view label);

// The list's upstream commit, for diagnostics.
std::string_view ListVersion();

}  // namespace torfox::psl

#endif  // TORFOX_CORE_URL_PUBLIC_SUFFIX_H_
