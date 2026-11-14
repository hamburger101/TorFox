// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_PRIVACY_HTTPS_FIRST_H_
#define TORFOX_CORE_PRIVACY_HTTPS_FIRST_H_

#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace torfox::https_first {

// Test builds only (--torfox-test-https-port=<http>:<https>): fixture servers
// listen on random ports, so an http URL on |http| is upgraded to |https|.
struct TestPorts {
  std::string http;
  std::string https;
};

// True for hosts TorFox tries over https first: public domain names, i.e. at
// least two labels ending in a top-level domain from the ICANN section of the
// Public Suffix List, and not a public suffix themselves. Not upgraded:
// IP addresses, "localhost", single-label (intranet) names, names under
// non-public TLDs (".local", ".test", ".internal", ...) and ".onion" (onion
// services are end-to-end encrypted, and handled by Tor).
bool IsUpgradableHost(std::string_view host);

// The https:// URL to try instead of |url|, or "" to leave |url| as it is.
// Only http:// URLs without user info or an explicit port (Chromium drops
// the default ":80") and with an upgradable host; the path, query and
// fragment are kept.
std::string UpgradedUrl(std::string_view url, const TestPorts* test_ports = nullptr);

// Whether a failed https attempt (Chromium net error code) should offer the
// http version. Not when the name does not resolve or the computer is
// offline (http would fail too), or when the load was stopped or replaced.
bool OffersHttpAfter(int net_error);

// The hosts the user chose to load over http, for the rest of the session
// (one set for normal windows, one per private window). Thread-safe.
class Exceptions {
 public:
  void Add(std::string_view host);
  bool Contains(std::string_view host) const;
  void Clear();

 private:
  mutable std::mutex lock_;
  std::set<std::string> hosts_;
};

}  // namespace torfox::https_first

#endif  // TORFOX_CORE_PRIVACY_HTTPS_FIRST_H_
