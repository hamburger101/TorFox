// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/nav_input.h"

#include "src/core/url/internal_url.h"

namespace torfox {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = Lower(c);
  }
  return out;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && IsSpace(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && IsSpace(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

bool IsHostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_';
}

// Extracts the host from "[scheme://][userinfo@]host[:port][/path]".
std::string HostOf(std::string_view token) {
  std::size_t scheme = token.find("://");
  if (scheme != std::string_view::npos) {
    token.remove_prefix(scheme + 3);
  }
  std::size_t end = token.find_first_of("/?#\\");
  std::string_view authority = token.substr(0, end);
  std::size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority.remove_prefix(at + 1);
  }
  if (!authority.empty() && authority.front() == '[') {
    return std::string();  // IPv6 literal; never an onion.
  }
  std::size_t colon = authority.find(':');
  return ToLower(authority.substr(0, colon));
}

bool HostIsOnion(std::string host) {
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  constexpr std::string_view kOnion = ".onion";
  return host.size() > kOnion.size() &&
         host.compare(host.size() - kOnion.size(), kOnion.size(), kOnion) == 0;
}

bool LooksLikeHost(std::string_view input) {
  // "user@example.com" is far more likely an e-mail address than a URL with
  // credentials; never turn it into a navigation.
  if (input.substr(0, input.find_first_of("/?#")).find('@') != std::string_view::npos) {
    return false;
  }
  std::string host = HostOf(input);
  if (host.empty()) {
    return false;
  }
  for (char c : host) {
    if (!IsHostChar(c)) {
      return false;
    }
  }
  if (host == "localhost") {
    return true;
  }
  std::size_t dot = host.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= host.size()) {
    return false;
  }
  // The last label must be alphabetic (a TLD) or the host must be all digits
  // and dots (an IPv4 address).
  bool tld_alpha = true;
  for (std::size_t i = dot + 1; i < host.size(); ++i) {
    char c = host[i];
    if (!(c >= 'a' && c <= 'z')) {
      tld_alpha = false;
    }
  }
  if (tld_alpha && host.size() - dot - 1 >= 2) {
    return true;
  }
  for (char c : host) {
    if (!((c >= '0' && c <= '9') || c == '.')) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool MentionsOnionHost(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSpace(text[i])) {
      ++i;
    }
    std::size_t start = i;
    while (i < text.size() && !IsSpace(text[i])) {
      ++i;
    }
    if (i > start) {
      std::string_view token = text.substr(start, i - start);
      // Strip punctuation a sentence might wrap around a host.
      while (!token.empty() && (token.front() == '(' || token.front() == '"' ||
                                token.front() == '\'' || token.front() == '<')) {
        token.remove_prefix(1);
      }
      while (!token.empty() && (token.back() == ')' || token.back() == '"' ||
                                token.back() == '\'' || token.back() == '>' ||
                                token.back() == ',' || token.back() == ';' ||
                                token.back() == '!' || token.back() == '?')) {
        token.remove_suffix(1);
      }
      if (HostIsOnion(HostOf(token))) {
        return true;
      }
    }
  }
  return false;
}

std::string DuckDuckGoSearchUrl(std::string_view query) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string url = "https://duckduckgo.com/?q=";
  for (char c : query) {
    unsigned char u = static_cast<unsigned char>(c);
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' ||
        u == '.' || u == '_' || u == '~') {
      url.push_back(c);
    } else {
      url.push_back('%');
      url.push_back(kHex[u >> 4]);
      url.push_back(kHex[u & 0x0F]);
    }
  }
  return url;
}

NavDecision ClassifyNavInput(std::string_view raw) {
  NavDecision d;
  if (raw.size() > kMaxNavInputLength) {
    d.reason = "input is too long";
    return d;
  }
  std::string_view input = Trim(raw);
  if (input.empty()) {
    d.reason = "input is empty";
    return d;
  }
  for (char c : input) {
    unsigned char u = static_cast<unsigned char>(c);
    if ((u < 0x20 && c != '\t') || u == 0x7F) {
      d.reason = "input contains control characters";
      return d;
    }
  }

  // Rule 2: onion hosts never reach the search engine or a non-Tor context.
  if (MentionsOnionHost(input)) {
    d.kind = NavKind::kOnion;
    d.url = std::string(input);
    d.reason = "onion addresses open only in Tor tabs";
    return d;
  }

  bool has_space = false;
  for (char c : input) {
    if (IsSpace(c)) {
      has_space = true;
      break;
    }
  }

  // Rule 3: explicit schemes.
  if (!has_space) {
    std::size_t colon = input.find(':');
    std::string scheme = colon == std::string_view::npos ? "" : ToLower(input.substr(0, colon));
    bool scheme_like = !scheme.empty();
    for (char c : scheme) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
            c == '.')) {
        scheme_like = false;
      }
    }
    if (scheme_like && input.substr(colon, 3) == "://") {
      if (scheme == "http" || scheme == "https") {
        if (HostOf(input).empty()) {
          d.reason = "URL has no host";
          return d;
        }
        d.kind = NavKind::kWebUrl;
        d.url = std::string(input);
        return d;
      }
      if (scheme == kInternalScheme) {
        auto parsed = ParseInternalUrl(ToLower(input));
        if (parsed && IsInternalPage(parsed->host) && parsed->host != "shell") {
          d.kind = NavKind::kInternal;
          d.url = InternalPageUrl(parsed->host);
          return d;
        }
        d.reason = "unknown TorFox page";
        return d;
      }
      d.reason = "the \"" + scheme + "\" scheme is not supported";
      return d;
    }
    if (scheme_like && (scheme == "javascript" || scheme == "data" || scheme == "file" ||
                        scheme == "vbscript" || scheme == "blob" || scheme == "about" ||
                        scheme == "chrome" || scheme == "devtools" || scheme == "view-source")) {
      d.reason = "the \"" + scheme + "\" scheme is not supported";
      return d;
    }
    // Rule 4: host-like input.
    if (LooksLikeHost(input)) {
      d.kind = NavKind::kWebUrl;
      d.url = "https://" + std::string(input);
      return d;
    }
  }

  // Rule 5: search.
  d.kind = NavKind::kSearch;
  d.url = DuckDuckGoSearchUrl(input);
  return d;
}

}  // namespace torfox
