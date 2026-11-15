// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/internal_url.h"

#include <array>

namespace torfox {

namespace {

constexpr std::array<std::string_view, 8> kPages = {"shell",     "newtab",    "settings",
                                                    "bookmarks", "history",   "downloads",
                                                    "suggest",   "whatsnew"};

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::span<const std::string_view> InternalPages() {
  return std::span<const std::string_view>(kPages);
}

bool IsInternalPage(std::string_view host) {
  for (std::string_view p : kPages) {
    if (p == host) {
      return true;
    }
  }
  return false;
}

bool IsTabPage(std::string_view host) {
  return IsInternalPage(host) && host != "shell" && host != "suggest";
}

std::optional<InternalUrl> ParseInternalUrl(std::string_view url) {
  constexpr std::string_view kPrefix = "torfox://";
  if (url.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  std::string_view rest = url.substr(kPrefix.size());
  std::size_t host_end = 0;
  while (host_end < rest.size() && rest[host_end] >= 'a' && rest[host_end] <= 'z') {
    ++host_end;
  }
  if (host_end == 0 || host_end > 32) {
    return std::nullopt;
  }
  InternalUrl out;
  out.host = std::string(rest.substr(0, host_end));
  rest = rest.substr(host_end);
  if (rest.empty()) {
    out.path = "/";
    return out;
  }
  if (rest[0] == '?' || rest[0] == '#') {
    out.path = "/";
    return out;
  }
  if (rest[0] != '/') {
    return std::nullopt;  // ':' (port), '@' (userinfo), '%', '\\', '.', etc.
  }
  std::size_t path_end = rest.find_first_of("?#");
  out.path = std::string(rest.substr(0, path_end));
  for (char c : out.path) {
    if (c == '\\' || static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E) {
      return std::nullopt;
    }
  }
  return out;
}

std::string InternalPageUrl(std::string_view host) {
  return std::string(kInternalScheme) + "://" + std::string(host) + "/";
}

std::optional<std::string> ResourcePathFor(const InternalUrl& url) {
  if (!IsInternalPage(url.host)) {
    return std::nullopt;
  }
  if (url.path == "/" || url.path == "/index.html") {
    return "pages/" + url.host + "/index.html";
  }
  constexpr std::string_view kAssets = "/assets/";
  if (url.path.size() <= kAssets.size() || url.path.compare(0, kAssets.size(), kAssets) != 0) {
    return std::nullopt;
  }
  std::string_view file = std::string_view(url.path).substr(kAssets.size());
  // Bundled asset names are flat: letters, digits, '-', '_', '.', no "..".
  if (file.find("..") != std::string_view::npos || file.front() == '.') {
    return std::nullopt;
  }
  for (char c : file) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.';
    if (!ok) {
      return std::nullopt;
    }
  }
  return "assets/" + std::string(file);
}

bool HasInternalScheme(std::string_view url) {
  // Skip leading C0 controls and spaces, which URL parsers strip.
  std::size_t i = 0;
  while (i < url.size() && static_cast<unsigned char>(url[i]) <= 0x20) {
    ++i;
  }
  std::string scheme;
  for (; i < url.size() && scheme.size() <= kInternalScheme.size(); ++i) {
    char c = url[i];
    if (c == '\t' || c == '\n' || c == '\r') {
      continue;  // Stripped by URL parsers anywhere in the URL.
    }
    if (c == ':') {
      return scheme == kInternalScheme;
    }
    scheme.push_back(ToLowerAscii(c));
  }
  return false;
}

}  // namespace torfox
