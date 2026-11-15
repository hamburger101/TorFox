// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/privacy/https_first.h"

#include "src/core/url/guards.h"
#include "src/core/url/public_suffix.h"

namespace torfox::https_first {

namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

std::string Normalize(std::string_view host) {
  std::string h = Lower(host);
  while (!h.empty() && h.back() == '.') {
    h.pop_back();
  }
  return h;
}

}  // namespace

bool IsUpgradableHost(std::string_view raw) {
  const std::string host = Normalize(raw);
  if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_") !=
                          std::string::npos) {
    return false;  // IPv6 literals, and anything that is not an ASCII domain name.
  }
  if (host.find_first_not_of("0123456789.") == std::string::npos) {
    return false;  // IPv4.
  }
  const std::size_t dot = host.rfind('.');
  if (dot == std::string::npos || host.find("..") != std::string::npos || host.front() == '.') {
    return false;  // Single label, or malformed.
  }
  if (guards::IsOnionHost(host)) {
    return false;
  }
  return psl::IsKnownTld(host.substr(dot + 1)) &&
         psl::RegistrableDomain(host, psl::Sections::kIcannOnly).has_value();
}

std::string UpgradedUrl(std::string_view url, const TestPorts* test_ports) {
  if (url.size() < 7 || Lower(url.substr(0, 7)) != "http://") {
    return std::string();
  }
  const std::string_view rest = url.substr(7);
  const std::size_t end = rest.find_first_of("/?#\\");
  const std::string_view authority = rest.substr(0, end);
  const std::string_view tail = end == std::string_view::npos ? std::string_view() : rest.substr(end);
  if (authority.find('@') != std::string_view::npos || authority.empty() ||
      authority.front() == '[') {
    return std::string();
  }
  const std::size_t colon = authority.find(':');
  const std::string_view host = authority.substr(0, colon);
  std::string port;
  if (colon != std::string_view::npos) {
    const std::string_view given = authority.substr(colon + 1);
    if (!test_ports || test_ports->http.empty() || given != test_ports->http) {
      return std::string();  // An explicit port: the site chose it; leave it alone.
    }
    port = ":" + test_ports->https;
  }
  if (!IsUpgradableHost(host)) {
    return std::string();
  }
  return "https://" + std::string(host) + port + std::string(tail.empty() ? "/" : tail);
}

bool OffersHttpAfter(int net_error) {
  switch (net_error) {
    case 0:     // OK
    case -3:    // ERR_ABORTED: stopped, or replaced by another navigation.
    case -20:   // ERR_BLOCKED_BY_CLIENT: TorFox itself refused it.
    case -105:  // ERR_NAME_NOT_RESOLVED
    case -106:  // ERR_INTERNET_DISCONNECTED
    case -137:  // ERR_NAME_RESOLUTION_FAILED
      return false;
    default:
      return net_error < 0;
  }
}

void Exceptions::Add(std::string_view host) {
  std::lock_guard<std::mutex> guard(lock_);
  hosts_.insert(Normalize(host));
}

bool Exceptions::Contains(std::string_view host) const {
  std::lock_guard<std::mutex> guard(lock_);
  return hosts_.count(Normalize(host)) > 0;
}

void Exceptions::Clear() {
  std::lock_guard<std::mutex> guard(lock_);
  hosts_.clear();
}

}  // namespace torfox::https_first
