// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/nav_input.h"

#include "src/core/url/internal_url.h"
#include "src/core/url/public_suffix.h"

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
  // ASCII letters, digits, '-', '_' and '.', plus any non-ASCII byte (IDN
  // labels typed in Unicode; Chromium converts them to punycode).
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool AllDigits(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// Value of a string of at most 5 decimal digits (checked by the caller).
int SmallNumber(std::string_view digits) {
  int value = 0;
  for (char c : digits) {
    value = value * 10 + (c - '0');
  }
  return value;
}

// Splits "[scheme://][userinfo@]host[:port][/path]" into host and port.
// |bracketed| is set for "[...]" IPv6 literals (|host| excludes brackets).
struct Authority {
  std::string host;  // Lower-case ASCII; non-ASCII bytes kept as is.
  std::string_view port;
  bool has_port = false;
  bool bracketed = false;
  bool valid = true;
};

Authority AuthorityOf(std::string_view token) {
  Authority a;
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
  std::string_view rest;
  if (!authority.empty() && authority.front() == '[') {
    std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      a.valid = false;
      return a;
    }
    a.bracketed = true;
    a.host = ToLower(authority.substr(1, close - 1));
    rest = authority.substr(close + 1);
    if (!rest.empty() && rest.front() != ':') {
      a.valid = false;
      return a;
    }
  } else {
    std::size_t colon = authority.find(':');
    a.host = ToLower(authority.substr(0, colon));
    rest = colon == std::string_view::npos ? std::string_view() : authority.substr(colon);
  }
  if (!rest.empty()) {
    a.has_port = true;
    a.port = rest.substr(1);
  }
  return a;
}

std::string HostOf(std::string_view token) {
  Authority a = AuthorityOf(token);
  return a.bracketed || !a.valid ? std::string() : a.host;
}

bool HostIsOnion(std::string host) {
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  constexpr std::string_view kOnion = ".onion";
  return host.size() > kOnion.size() &&
         host.compare(host.size() - kOnion.size(), kOnion.size(), kOnion) == 0;
}

// "a.b.c.d" with exactly four decimal octets 0-255.
bool ParseIpv4(std::string_view host, int octets[4]) {
  int count = 0;
  std::size_t start = 0;
  while (true) {
    std::size_t dot = host.find('.', start);
    std::string_view part = host.substr(start, dot == std::string_view::npos ? dot : dot - start);
    if (count == 4 || !AllDigits(part) || part.size() > 3) {
      return false;
    }
    const int value = SmallNumber(part);
    if (value > 255) {
      return false;
    }
    octets[count++] = value;
    if (dot == std::string_view::npos) {
      return count == 4;
    }
    start = dot + 1;
  }
}

bool LooksLikeIpv6(std::string_view host) {
  if (host.find(':') == std::string_view::npos) {
    return false;
  }
  for (char c : host) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool IsLocalhostName(std::string_view host) {
  constexpr std::string_view kLocal = ".localhost";
  return host == "localhost" ||
         (host.size() > kLocal.size() && host.substr(host.size() - kLocal.size()) == kLocal);
}

// Hosts that normally have no TLS certificate: loopback, private and
// link-local addresses, and localhost names. Typed input for these gets
// http://; everything else gets https://.
bool IsLocalHost(const Authority& a) {
  if (a.bracketed) {
    const std::string& h = a.host;
    return h == "::1" || h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0 || h.rfind("fe8", 0) == 0 ||
           h.rfind("fe9", 0) == 0 || h.rfind("fea", 0) == 0 || h.rfind("feb", 0) == 0;
  }
  if (IsLocalhostName(a.host)) {
    return true;
  }
  int o[4];
  if (!ParseIpv4(a.host, o)) {
    return false;
  }
  return o[0] == 127 || o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
         (o[0] == 192 && o[1] == 168) || (o[0] == 169 && o[1] == 254);
}

// Decides whether scheme-less, space-free |input| names a host to visit.
bool LooksLikeHost(std::string_view input, Authority* out) {
  // "user@example.com" is far more likely an e-mail address than a URL with
  // credentials; never turn it into a navigation.
  const std::string_view before_path = input.substr(0, input.find_first_of("/?#"));
  if (before_path.find('@') != std::string_view::npos) {
    return false;
  }
  Authority a = AuthorityOf(input);
  if (!a.valid || a.host.empty()) {
    return false;
  }
  if (a.has_port && (!AllDigits(a.port) || a.port.size() > 5 || SmallNumber(a.port) > 65535)) {
    return false;
  }
  *out = a;
  if (a.bracketed) {
    return LooksLikeIpv6(a.host);
  }
  for (char c : a.host) {
    if (!IsHostChar(c)) {
      return false;
    }
  }
  std::string_view host = a.host;
  if (host.back() == '.') {
    host.remove_suffix(1);  // "example.com." is fully qualified.
  }
  if (host.empty() || host.front() == '.' || host.find("..") != std::string_view::npos) {
    return false;
  }
  int octets[4];
  if (IsLocalhostName(host) || ParseIpv4(host, octets)) {
    return true;
  }
  const std::size_t dot = host.rfind('.');
  if (dot == std::string_view::npos) {
    // A single label ("nas") is a host only with an explicit port.
    return a.has_port;
  }
  const std::string_view tld = host.substr(dot + 1);
  if (AllDigits(tld)) {
    return false;  // "1.5", "3.14.15": numbers, not hosts.
  }
  // A known top-level domain, or an explicit port or path ("nas.lan:8080",
  // "nas.lan/") for local names the public list does not know.
  return psl::IsKnownTld(tld) || a.has_port || before_path.size() < input.size();
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
        if (Authority a = AuthorityOf(input); !a.valid || a.host.empty()) {
          d.reason = "URL has no host";
          return d;
        }
        d.kind = NavKind::kWebUrl;
        d.url = std::string(input);
        return d;
      }
      if (scheme == kInternalScheme) {
        auto parsed = ParseInternalUrl(ToLower(input));
        if (parsed && IsTabPage(parsed->host)) {
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
    Authority authority;
    if (LooksLikeHost(input, &authority)) {
      d.kind = NavKind::kWebUrl;
      d.url = (IsLocalHost(authority) ? "http://" : "https://") + std::string(input);
      return d;
    }
  }

  // Rule 5: search.
  d.kind = NavKind::kSearch;
  d.url = DuckDuckGoSearchUrl(input);
  return d;
}

}  // namespace torfox
