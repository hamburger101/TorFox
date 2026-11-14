// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/guards.h"

#include <cctype>

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
  if ((scheme == "http" || scheme == "https") && IsOnionHost(HostOf(url))) {
    return "onion host outside Tor";
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
  if (IsHttpLike(scheme) && IsOnionHost(HostOf(url))) {
    return "onion host outside Tor";
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
