// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/guards.h"

#include <array>
#include <cctype>
#include <optional>

#include "src/core/url/internal_url.h"

namespace torfox::guards {

namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool IsHttpLike(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss";
}

// blob: and filesystem: URLs carry their origin's URL inside them
// ("blob:https://a.test/uuid"); "" for any other scheme.
std::string_view InnerUrl(std::string_view scheme, std::string_view url) {
  if (scheme != "blob" && scheme != "filesystem") {
    return std::string_view();
  }
  return url.substr(scheme.size() + 1);
}

bool ReachesPrivilegedScheme(std::string_view scheme, std::string_view url) {
  if (HasInternalScheme(url) || IsPrivilegedScheme(scheme)) {
    return true;
  }
  const std::string_view inner = InnerUrl(scheme, url);
  return !inner.empty() && (HasInternalScheme(inner) || IsPrivilegedScheme(SchemeOf(inner)));
}

}  // namespace

bool IsInternal(Role role) {
  return role == Role::kShell || role == Role::kInternalTab;
}

std::string SchemeOf(std::string_view url) {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return std::string();
  }
  return Lower(url.substr(0, colon));
}

std::string HostOf(std::string_view url) {
  const std::size_t sep = url.find("://");
  if (sep == std::string_view::npos) {
    return std::string();
  }
  std::string_view rest = url.substr(sep + 3);
  const std::size_t end = rest.find_first_of("/?#\\");
  std::string_view authority = rest.substr(0, end);
  const std::size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority.remove_prefix(at + 1);
  }
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    return close == std::string_view::npos ? std::string() : Lower(authority.substr(1, close - 1));
  }
  const std::size_t colon = authority.find(':');
  return Lower(authority.substr(0, colon));
}

bool IsOnionHost(std::string_view host) {
  std::string h = Lower(host);
  while (!h.empty() && h.back() == '.') {
    h.pop_back();
  }
  constexpr std::string_view kOnion = ".onion";
  return h.size() > kOnion.size() && h.compare(h.size() - kOnion.size(), kOnion.size(), kOnion) == 0;
}

bool IsPrivilegedScheme(std::string_view scheme) {
  const std::string s = Lower(scheme);
  return s == kInternalScheme || s == "chrome" || s == "chrome-untrusted" || s == "devtools" ||
         s == "chrome-extension" || s == "chrome-search" || s == "chrome-error" || s == "file" ||
         s == "filesystem" || s == "view-source";
}

namespace {

std::optional<std::array<int, 4>> Ipv4Literal(std::string_view host) {
  std::array<int, 4> out{};
  int part = 0;
  std::size_t i = 0;
  while (part < 4) {
    std::size_t len = 0;
    int v = 0;
    while (i + len < host.size() && host[i + len] >= '0' && host[i + len] <= '9' && len < 4) {
      v = v * 10 + (host[i + len] - '0');
      ++len;
    }
    if (len == 0 || len > 3 || v > 255) {
      return std::nullopt;
    }
    out[part++] = v;
    i += len;
    if (part < 4) {
      if (i >= host.size() || host[i] != '.') {
        return std::nullopt;
      }
      ++i;
    }
  }
  if (i != host.size()) {
    return std::nullopt;
  }
  return out;
}

bool LocalIpv4(const std::array<int, 4>& a) {
  return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) ||
         (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
         (a[0] == 100 && a[1] >= 64 && a[1] <= 127);
}

}  // namespace

bool IsLocalNetworkHost(std::string_view host_in) {
  std::string host = Lower(host_in);
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  if (host.empty()) {
    return false;
  }
  if (host == "localhost" || (host.size() > 10 && host.ends_with(".localhost"))) {
    return true;
  }
  if (std::optional<std::array<int, 4>> v4 = Ipv4Literal(host)) {
    return LocalIpv4(*v4);
  }
  if (host.find(':') == std::string::npos) {
    return false;
  }
  // IPv6 (canonical, as Chromium writes it).
  if (host == "::" || host == "::1") {
    return true;
  }
  constexpr std::string_view kMapped = "::ffff:";
  if (host.rfind(kMapped, 0) == 0) {
    std::string_view rest = std::string_view(host).substr(kMapped.size());
    if (std::optional<std::array<int, 4>> v4 = Ipv4Literal(rest)) {
      return LocalIpv4(*v4);
    }
    // Chromium writes mapped addresses in hex ("::ffff:7f00:1"): treat any
    // mapped address as local rather than decode it.
    return true;
  }
  const std::string first = host.substr(0, host.find(':'));
  if (first.size() == 4 && (first.rfind("fc", 0) == 0 || first.rfind("fd", 0) == 0)) {
    return true;  // fc00::/7, unique local.
  }
  if (first.size() == 4 && first.rfind("fe", 0) == 0 &&
      (first[2] == '8' || first[2] == '9' || first[2] == 'a' || first[2] == 'b')) {
    return true;  // fe80::/10, link-local.
  }
  return false;
}

std::string NavigationBlockReason(Role role, std::string_view url) {
  const std::string scheme = SchemeOf(url);
  if (IsInternal(role)) {
    auto parsed = ParseInternalUrl(url);
    if (!parsed || !IsInternalPage(parsed->host)) {
      return "internal browsers only show TorFox pages";
    }
    if ((parsed->host == "shell") != (role == Role::kShell)) {
      return "page not allowed for this browser";
    }
    return std::string();
  }
  if (ReachesPrivilegedScheme(scheme, url)) {
    return "privileged scheme";
  }
  if (scheme == "about") {
    const std::string lower = Lower(url);
    return lower == "about:blank" || lower == "about:srcdoc" ? std::string() : "about: page";
  }
  if (scheme != "http" && scheme != "https" && scheme != "data" && scheme != "blob") {
    return "scheme not allowed";
  }
  if ((scheme == "http" || scheme == "https") && role != Role::kTorTab &&
      IsOnionHost(HostOf(url))) {
    return "onion host outside Tor";
  }
  if ((scheme == "http" || scheme == "https") && role == Role::kTorTab &&
      IsLocalNetworkHost(HostOf(url))) {
    return "local address in a Tor window";
  }
  return std::string();
}

std::string ResourceBlockReason(Role role, std::string_view url) {
  const std::string scheme = SchemeOf(url);
  if (IsInternal(role)) {
    if (scheme == kInternalScheme || scheme == "data" ||
        (scheme == "blob" && SchemeOf(InnerUrl(scheme, url)) == kInternalScheme)) {
      return std::string();
    }
    return "network access from internal page";
  }
  if (ReachesPrivilegedScheme(scheme, url)) {
    return "privileged scheme";
  }
  if (IsHttpLike(scheme) && role != Role::kTorTab && IsOnionHost(HostOf(url))) {
    return "onion host outside Tor";
  }
  if (IsHttpLike(scheme) && role == Role::kTorTab && IsLocalNetworkHost(HostOf(url))) {
    return "local address in a Tor window";
  }
  return std::string();
}

InternalRequest DecideInternalRequest(bool known, Role role, std::string_view method,
                                      std::string_view url) {
  if (!known || !IsInternal(role)) {
    return {403, "not an internal browser", {}};
  }
  if (method != "GET") {
    return {405, "method", {}};
  }
  auto parsed = ParseInternalUrl(url);
  if (!parsed || !IsInternalPage(parsed->host)) {
    return {404, "unknown page", {}};
  }
  if ((parsed->host == "shell") != (role == Role::kShell)) {
    return {403, "page not allowed for role", {}};
  }
  auto path = ResourcePathFor(*parsed);
  if (!path) {
    return {404, "no such resource", {}};
  }
  return {200, "ok", *path};
}

}  // namespace torfox::guards
