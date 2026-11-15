// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/adblock/filters.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/core/privacy/trackers.h"
#include "src/core/url/public_suffix.h"

namespace torfox::adblock {

namespace {

#include "adblock_data.inc"  // kEasyListCommit, kEasyListRules.

constexpr std::size_t npos = std::string_view::npos;

constexpr std::uint32_t kRequestTypes = kScript | kImage | kStylesheet | kObject |
                                        kXmlHttpRequest | kSubdocument | kPing | kWebSocket |
                                        kFont | kMedia | kOther;

enum Flag : std::uint8_t {
  kHostAnchor = 1,   // "||"
  kLeftAnchor = 2,   // "|" at the start
  kRightAnchor = 4,  // "|" at the end
  kMatchCase = 8,
  kImportant = 16,
};

enum Party : std::uint8_t { kAnyParty, kFirstParty, kThirdParty };

enum PageFlag : std::uint8_t {
  kPageDocument = 1,
  kPageElemHide = 2,
  kPageGenericHide = 4,
  kPageGenericBlock = 8,
};

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Characters a URL token is made of (both in rules and in addresses).
bool IsTokenChar(char c) {
  return IsAlnum(c) || c == '%';
}

// What "^" matches: anything but a letter, a digit, or one of _-.%
bool IsSeparator(char c) {
  return !(IsAlnum(c) || c == '_' || c == '-' || c == '.' || c == '%');
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = Lower(c);
  }
  return out;
}

bool HasUpper(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

// |lower| (lowercase) equals |any| (any case).
bool EqualsFolded(std::string_view lower, std::string_view any) {
  if (lower.size() != any.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lower.size(); ++i) {
    if (lower[i] != Lower(any[i])) {
      return false;
    }
  }
  return true;
}

std::uint64_t HashFolded(std::string_view s) {
  std::uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(Lower(c));
    h *= 1099511628211ull;
  }
  return h;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  while (true) {
    const std::size_t at = s.find(sep);
    out.push_back(s.substr(0, at));
    if (at == npos) {
      return out;
    }
    s.remove_prefix(at + 1);
  }
}

// ----- addresses -------------------------------------------------------------

struct Url {
  std::string exact;  // The address (a trailing dot of the host removed).
  std::string lower;  // The same, lowercase.
  std::size_t host_begin = 0;
  std::size_t host_end = 0;
  std::string_view host() const {
    return std::string_view(lower).substr(host_begin, host_end - host_begin);
  }
};

bool ParseUrl(std::string_view url, Url* out) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == npos || scheme_end == 0) {
    return false;
  }
  std::size_t begin = scheme_end + 3;
  std::size_t authority_end = url.find_first_of("/?#", begin);
  if (authority_end == npos) {
    authority_end = url.size();
  }
  const std::size_t at = url.substr(begin, authority_end - begin).rfind('@');
  if (at != npos) {
    begin += at + 1;
  }
  std::size_t end = 0;
  if (begin < authority_end && url[begin] == '[') {
    end = url.find(']', begin);
    if (end == npos || end >= authority_end) {
      return false;
    }
    ++end;
  } else {
    end = url.find(':', begin);
    if (end == npos || end > authority_end) {
      end = authority_end;
    }
  }
  if (end <= begin) {
    return false;
  }
  out->exact = std::string(url);
  // "ads.example.com." is ads.example.com.
  if (end - begin > 1 && out->exact[end - 1] == '.') {
    out->exact.erase(end - 1, 1);
    --end;
  }
  out->lower = ToLower(out->exact);
  out->host_begin = begin;
  out->host_end = end;
  return true;
}

// ----- domain lists ----------------------------------------------------------

struct DomainEntry {
  std::string_view name;  // "example.com", or "example.*" (any public suffix).
  bool exclude = false;   // "~example.com"
};

bool ValidDomainName(std::string_view name) {
  if (name.size() > 2 && name.ends_with(".*")) {
    name.remove_suffix(2);
  }
  if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return IsAlnum(c) || c == '.' || c == '-' || c == '_';
  });
}

bool ParseDomains(std::string_view list, char sep, std::vector<DomainEntry>* out) {
  for (std::string_view item : Split(list, sep)) {
    DomainEntry e;
    if (!item.empty() && item.front() == '~') {
      e.exclude = true;
      item.remove_prefix(1);
    }
    if (!ValidDomainName(item)) {
      return false;
    }
    e.name = item;
    out->push_back(e);
  }
  return true;
}

// A host (lowercase) and, when first needed, its public suffix.
class HostInfo {
 public:
  explicit HostInfo(std::string_view host) : host_(host) {}
  std::string_view host() const { return host_; }
  std::string_view public_suffix() const {
    if (!suffix_) {
      std::optional<std::string> ps = psl::PublicSuffix(host_);
      suffix_ = ps ? *ps : std::string();
    }
    return *suffix_;
  }

 private:
  std::string_view host_;
  mutable std::optional<std::string> suffix_;
};

// |host| (lowercase) is |domain| (any case) or one of its subdomains.
bool HostIn(std::string_view host, std::string_view domain) {
  if (domain.empty() || host.size() < domain.size() ||
      !EqualsFolded(host.substr(host.size() - domain.size()), domain)) {
    return false;
  }
  return host.size() == domain.size() || host[host.size() - domain.size() - 1] == '.';
}

// How specific the match of |name| on |host| is (0: no match).
std::size_t EntryMatch(std::string_view name, const HostInfo& host) {
  if (name.size() > 2 && name.ends_with(".*")) {
    const std::string_view suffix = host.public_suffix();
    if (suffix.empty()) {
      return 0;
    }
    std::string full(name.substr(0, name.size() - 1));  // "example."
    full += suffix;
    return HostIn(host.host(), full) ? full.size() : 0;
  }
  return HostIn(host.host(), name) ? name.size() : 0;
}

bool HasInclude(std::span<const DomainEntry> entries) {
  return std::any_of(entries.begin(), entries.end(), [](const DomainEntry& e) { return !e.exclude; });
}

// Whether a rule limited to |entries| applies on |host| (null: not known).
// The most specific matching entry decides; when none matches, the rule
// applies only if it names no sites it is limited to.
bool DomainsApply(std::span<const DomainEntry> entries, const HostInfo* host) {
  if (entries.empty()) {
    return true;
  }
  std::size_t best = 0;
  bool best_excludes = false;
  if (host && !host->host().empty()) {
    for (const DomainEntry& e : entries) {
      const std::size_t len = EntryMatch(e.name, *host);
      if (len > best || (len > 0 && len == best && e.exclude)) {
        best = len;
        best_excludes = e.exclude;
      }
    }
  }
  if (best == 0) {
    return !HasInclude(entries);
  }
  return !best_excludes;
}

// ----- request rules ---------------------------------------------------------

struct NetRule {
  std::string_view text;
  std::string_view pattern;  // Without anchors and options.
  std::uint32_t types = kRequestTypes;
  std::uint32_t domains_begin = 0;
  std::uint16_t domains_count = 0;
  std::uint8_t party = kAnyParty;
  std::uint8_t flags = 0;
  std::uint8_t page = 0;  // Exceptions only: PageFlag.
};

std::uint32_t TypeNamed(std::string_view name) {
  struct Named {
    std::string_view name;
    std::uint32_t type;
  };
  static constexpr Named kTypes[] = {
      {"script", kScript},       {"image", kImage},
      {"stylesheet", kStylesheet}, {"css", kStylesheet},
      {"object", kObject},       {"xmlhttprequest", kXmlHttpRequest},
      {"xhr", kXmlHttpRequest},  {"subdocument", kSubdocument},
      {"frame", kSubdocument},   {"ping", kPing},
      {"beacon", kPing},         {"websocket", kWebSocket},
      {"font", kFont},           {"media", kMedia},
      {"other", kOther},         {"popup", kPopup},
  };
  for (const Named& t : kTypes) {
    if (t.name == name) {
      return t.type;
    }
  }
  return 0;
}

enum class Kind { kIgnored, kBlock, kException };

Kind ParseRequestRule(std::string_view text, NetRule* r, std::vector<DomainEntry>* domains) {
  std::string_view body = text;
  const bool exception = body.starts_with("@@");
  if (exception) {
    body.remove_prefix(2);
  }
  std::string_view options;
  const std::size_t dollar = body.rfind('$');
  if (dollar != npos) {
    options = body.substr(dollar + 1);
    body = body.substr(0, dollar);
    if (options.empty()) {
      return Kind::kIgnored;
    }
  }
  if (body.size() >= 2 && body.front() == '/' && body.back() == '/') {
    return Kind::kIgnored;  // A regular expression.
  }
  if (body.starts_with("||")) {
    r->flags |= kHostAnchor;
    body.remove_prefix(2);
  } else if (body.starts_with("|")) {
    r->flags |= kLeftAnchor;
    body.remove_prefix(1);
  }
  if (body.ends_with("|")) {
    r->flags |= kRightAnchor;
    body.remove_suffix(1);
  }
  if (body.find('|') != npos || ((r->flags & kHostAnchor) && body.empty())) {
    return Kind::kIgnored;
  }
  r->text = text;
  r->pattern = body;

  const std::size_t domains_start = domains->size();
  auto ignore = [&] {
    domains->resize(domains_start);
    return Kind::kIgnored;
  };
  std::uint32_t include_types = 0;
  std::uint32_t exclude_types = 0;
  bool document = false;
  bool seen_domain = false;
  if (!options.empty()) {
    for (std::string_view option : Split(options, ',')) {
      const bool negated = !option.empty() && option.front() == '~';
      std::string_view name = negated ? option.substr(1) : option;
      std::string_view value;
      const std::size_t eq = name.find('=');
      if (eq != npos) {
        value = name.substr(eq + 1);
        name = name.substr(0, eq);
        if (name != "domain" || negated || seen_domain || !ParseDomains(value, '|', domains)) {
          return ignore();
        }
        seen_domain = true;
        continue;
      }
      if (const std::uint32_t type = TypeNamed(name)) {
        (negated ? exclude_types : include_types) |= type;
      } else if (name == "third-party" || name == "3p") {
        r->party = negated ? kFirstParty : kThirdParty;
      } else if (name == "first-party" || name == "1p") {
        r->party = negated ? kThirdParty : kFirstParty;
      } else if (negated) {
        return ignore();
      } else if (name == "match-case") {
        r->flags |= kMatchCase;
      } else if (name == "important") {
        r->flags |= kImportant;
      } else if (name == "document" || name == "doc") {
        document = true;
      } else if (name == "elemhide" || name == "ehide") {
        r->page |= kPageElemHide;
      } else if (name == "generichide" || name == "ghide") {
        r->page |= kPageGenericHide;
      } else if (name == "genericblock") {
        r->page |= kPageGenericBlock;
      } else {
        return ignore();  // Not understood: never applied.
      }
    }
  }
  if (document) {
    r->page |= kPageDocument;
  }
  if (include_types) {
    r->types = include_types & ~exclude_types;
  } else if (exclude_types) {
    r->types = kRequestTypes & ~exclude_types;
  } else if (r->page) {
    r->types = 0;  // Only about whole documents.
  }
  r->domains_begin = static_cast<std::uint32_t>(domains_start);
  r->domains_count = static_cast<std::uint16_t>(domains->size() - domains_start);
  if (domains->size() - domains_start > 0xFFFF) {
    return ignore();
  }
  // A rule matching every address must at least be limited to some sites.
  const std::span<const DomainEntry> entries(domains->data() + domains_start, r->domains_count);
  if (r->pattern.find_first_not_of('*') == npos && !(r->flags & kHostAnchor) && !HasInclude(entries)) {
    return ignore();
  }
  if (!exception) {
    // A blocking rule only blocks requests (never a page the user opens).
    if (r->page & ~kPageDocument) {
      return ignore();
    }
    r->page = 0;
    if (r->types == 0) {
      return ignore();
    }
    return Kind::kBlock;
  }
  if (r->types == 0 && r->page == 0) {
    return ignore();
  }
  return Kind::kException;
}

// Matches |seg| (no "*") at |at| in |url|; sets |*end| to where it ends.
bool SegmentAt(std::string_view seg, std::string_view url, std::size_t at, bool fold,
               std::size_t* end) {
  std::size_t i = at;
  for (char pc : seg) {
    if (pc == '^') {
      if (i == url.size()) {
        continue;  // "^" also matches the end of the address.
      }
      if (!IsSeparator(url[i])) {
        return false;
      }
      ++i;
      continue;
    }
    if (i >= url.size() || (fold ? Lower(pc) : pc) != url[i]) {
      return false;
    }
    ++i;
  }
  *end = i;
  return true;
}

// The leftmost match of |seg| at or after |from|.
bool FindSegment(std::string_view seg, std::string_view url, std::size_t from, bool fold,
                 std::size_t* begin, std::size_t* end) {
  const char first = fold ? Lower(seg.front()) : seg.front();
  for (std::size_t s = from; s <= url.size(); ++s) {
    if (first != '^') {
      s = url.find(first, s);
      if (s == npos) {
        return false;
      }
    }
    if (SegmentAt(seg, url, s, fold, end)) {
      *begin = s;
      return true;
    }
  }
  return false;
}

// Matches a pattern ("*" wildcards, "^" separators) against |url|, from
// |start| if |anchored|, or anywhere after it; with |right_anchor| the match
// must end at the end of the address.
bool MatchPattern(std::string_view pat, std::string_view url, std::size_t start, bool anchored,
                  bool right_anchor, bool fold) {
  std::size_t pos = start;
  bool first = true;
  while (true) {
    const std::size_t star = pat.find('*');
    const bool last = star == npos;
    const std::string_view seg = last ? pat : pat.substr(0, star);
    if (!seg.empty()) {
      std::size_t b = 0;
      std::size_t e = 0;
      if (first && anchored) {
        if (!SegmentAt(seg, url, pos, fold, &e) || (last && right_anchor && e != url.size())) {
          return false;
        }
      } else if (last && right_anchor) {
        std::size_t from = pos;
        while (FindSegment(seg, url, from, fold, &b, &e)) {
          if (e == url.size()) {
            return true;
          }
          from = b + 1;
        }
        return false;
      } else if (!FindSegment(seg, url, pos, fold, &b, &e)) {
        return false;
      }
      pos = e;
    }
    if (last) {
      return true;
    }
    first = false;
    pat.remove_prefix(star + 1);
  }
}

bool MatchRule(const NetRule& r, const Url& u, std::size_t anchor = npos) {
  const bool fold = !(r.flags & kMatchCase);
  const std::string_view s = fold ? u.lower : u.exact;
  const bool right = (r.flags & kRightAnchor) != 0;
  if (anchor != npos) {
    return MatchPattern(r.pattern, s, anchor, true, right, fold);
  }
  if (r.flags & kHostAnchor) {
    // At the start of the host or of any of its labels.
    for (std::size_t p = u.host_begin; p < u.host_end;) {
      if (MatchPattern(r.pattern, s, p, true, right, fold)) {
        return true;
      }
      const std::size_t dot = u.lower.find('.', p);
      if (dot == npos || dot >= u.host_end) {
        break;
      }
      p = dot + 1;
    }
    return false;
  }
  return MatchPattern(r.pattern, s, 0, (r.flags & kLeftAnchor) != 0, right, fold);
}

// The whole host a "||host^..." rule is for, if it names one exactly.
std::string_view HostKey(const NetRule& r) {
  if (!(r.flags & kHostAnchor)) {
    return {};
  }
  const std::string_view p = r.pattern;
  std::size_t i = 0;
  while (i < p.size() && ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= '0' && p[i] <= '9') ||
                          p[i] == '.' || p[i] == '-')) {
    ++i;
  }
  if (i == 0 || i == p.size() || (p[i] != '^' && p[i] != '/' && p[i] != ':')) {
    return {};
  }
  const std::string_view host = p.substr(0, i);
  if (host.front() == '.' || host.back() == '.') {
    return {};
  }
  return host;
}

struct Token {
  std::uint64_t hash;
  std::size_t length;
};

// The tokens an address must contain, whole, for |r| to match it.
std::vector<Token> TokensOf(const NetRule& r) {
  std::vector<Token> out;
  const std::string_view p = r.pattern;
  std::size_t i = 0;
  while (i < p.size()) {
    if (!IsTokenChar(p[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < p.size() && IsTokenChar(p[j])) {
      ++j;
    }
    const bool left = i > 0 ? p[i - 1] != '*' : (r.flags & (kHostAnchor | kLeftAnchor)) != 0;
    const bool right = j < p.size() ? p[j] != '*' : (r.flags & kRightAnchor) != 0;
    if (left && right && j - i >= 2) {
      out.push_back(Token{HashFolded(p.substr(i, j - i)), j - i});
    }
    i = j;
  }
  return out;
}

// Request rules, found by the host they name, by a token, or (the few with
// neither) by trying each.
struct Index {
  std::unordered_map<std::string_view, std::vector<std::uint32_t>> by_host;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_token;
  std::vector<std::uint32_t> rest;

  // Calls |f(rule, anchor)| for each candidate rule until it returns true;
  // |anchor| is where a host-indexed rule must match (else npos).
  template <typename F>
  bool Any(const Url& u, F f) const {
    if (!by_host.empty()) {
      const std::string_view host = u.host();
      for (std::size_t p = 0; p < host.size();) {
        const auto it = by_host.find(host.substr(p));
        if (it != by_host.end()) {
          for (std::uint32_t id : it->second) {
            if (f(id, u.host_begin + p)) {
              return true;
            }
          }
        }
        const std::size_t dot = host.find('.', p);
        if (dot == npos) {
          break;
        }
        p = dot + 1;
      }
    }
    if (!by_token.empty()) {
      std::vector<std::uint64_t> seen;
      const std::string_view s = u.lower;
      std::size_t i = 0;
      while (i < s.size()) {
        if (!IsTokenChar(s[i])) {
          ++i;
          continue;
        }
        std::size_t j = i;
        while (j < s.size() && IsTokenChar(s[j])) {
          ++j;
        }
        if (j - i >= 2) {
          const std::uint64_t h = HashFolded(s.substr(i, j - i));
          if (std::find(seen.begin(), seen.end(), h) == seen.end()) {
            seen.push_back(h);
            const auto it = by_token.find(h);
            if (it != by_token.end()) {
              for (std::uint32_t id : it->second) {
                if (f(id, npos)) {
                  return true;
                }
              }
            }
          }
        }
        i = j;
      }
    }
    for (std::uint32_t id : rest) {
      if (f(id, npos)) {
        return true;
      }
    }
    return false;
  }

  std::size_t Bytes() const {
    std::size_t n = rest.capacity() * sizeof(std::uint32_t);
    for (const auto& [k, v] : by_host) {
      n += 48 + v.capacity() * sizeof(std::uint32_t);
    }
    for (const auto& [k, v] : by_token) {
      n += 40 + v.capacity() * sizeof(std::uint32_t);
    }
    return n;
  }
};

// What a request is checked against.
struct RequestContext {
  const Url* url = nullptr;
  const HostInfo* host = nullptr;  // The document's host (domain=, third-party).
  mutable int third_party = -1;

  bool ThirdParty() const {
    if (third_party < 0) {
      third_party = (!host || host->host().empty() ||
                     trackers::SiteOf(url->host()) != trackers::SiteOf(host->host()))
                        ? 1
                        : 0;
    }
    return third_party == 1;
  }
};

// ----- element hiding --------------------------------------------------------

struct HideRule {
  std::string_view selector;
  std::uint32_t domains_begin = 0;
  std::uint16_t domains_count = 0;
  bool exception = false;
};

// Scriptlets ("##+js(...)"), HTML filters ("##^...") and procedural
// selectors look like selectors but are not CSS.
bool PlainCssSelector(std::string_view selector) {
  static constexpr std::string_view kProcedural[] = {
      ":-abp-",       ":has-text(",        ":xpath(",      ":upward(",
      ":remove(",     ":style(",           ":matches-",    ":min-text-length(",
      ":watch-attr(", ":others(",          ":nth-ancestor(", ":remove-attr(",
      ":remove-class(", ":if(",            ":if-not(",     ":contains(",
  };
  if (selector.empty() || selector.front() == '+' || selector.front() == '^') {
    return false;
  }
  for (std::string_view p : kProcedural) {
    if (selector.find(p) != npos) {
      return false;
    }
  }
  return true;
}

void AppendHide(std::string* css, std::string_view selector) {
  css->append(selector);
  css->append("{display:none!important}\n");
}

}  // namespace

bool SafeSelector(std::string_view s) {
  if (s.empty() || s.size() > 4096) {
    return false;
  }
  char quote = 0;
  int round = 0;
  int square = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
      return false;
    }
    if (c == '\\') {
      if (i + 1 >= s.size() || static_cast<unsigned char>(s[i + 1]) < 0x20) {
        return false;
      }
      ++i;
      continue;
    }
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    switch (c) {
      case '"':
      case '\'':
        quote = c;
        break;
      case '(':
        ++round;
        break;
      case ')':
        if (--round < 0) {
          return false;
        }
        break;
      case '[':
        ++square;
        break;
      case ']':
        if (--square < 0) {
          return false;
        }
        break;
      case '{':
      case '}':
      case ';':
      case '@':
      case '<':
      case '!':
        return false;
      case '/':
        if (i + 1 < s.size() && s[i + 1] == '*') {
          return false;
        }
        break;
      case '*':
        if (i + 1 < s.size() && s[i + 1] == '/') {
          return false;
        }
        break;
      default:
        break;
    }
  }
  return quote == 0 && round == 0 && square == 0;
}

struct Engine::Impl {
  std::vector<NetRule> rules;
  std::vector<DomainEntry> domains;
  Index blocks;
  Index exceptions;
  Index pages;  // Exceptions about whole documents.

  std::vector<HideRule> hides;
  std::deque<std::string> owned;  // Lowercase copies of domain names.
  std::unordered_map<std::string_view, std::vector<std::uint32_t>> hides_by_domain;
  std::vector<std::uint32_t> hides_everywhere_but;  // Only "~domain" entries.
  std::vector<std::string_view> generic;
  std::unordered_set<std::string_view> generic_exceptions;
  std::string generic_css;
  bool entity_domains = false;

  std::size_t request_rules = 0;
  std::size_t hiding_rules = 0;
  std::size_t ignored = 0;

  std::span<const DomainEntry> EntriesOf(std::uint32_t begin, std::uint16_t count) const {
    return std::span<const DomainEntry>(domains.data() + begin, count);
  }

  bool AddHiding(std::string_view text);
  void Build(const std::vector<std::pair<std::uint32_t, Kind>>& added);
  const NetRule* Find(const Index& index, const RequestContext& ctx, std::uint32_t type,
                      bool skip_generic, bool important_only) const;
  PageOptions Page(std::string_view page_url, std::string_view context_url) const;
};

bool Engine::Impl::AddHiding(std::string_view text) {
  const std::size_t plain = text.find("##");
  const std::size_t except = text.find("#@#");
  const bool exception = except != npos && (plain == npos || except < plain);
  const std::size_t sep = exception ? except : plain;
  const std::string_view selector = text.substr(sep + (exception ? 3 : 2));
  const std::string_view sites = text.substr(0, sep);
  if (!PlainCssSelector(selector) || !SafeSelector(selector)) {
    return false;
  }
  if (sites.empty()) {
    if (exception) {
      generic_exceptions.insert(selector);
    } else {
      generic.push_back(selector);
    }
    return true;
  }
  const std::size_t first = domains.size();
  if (!ParseDomains(sites, ',', &domains) || domains.size() - first > 0xFFFF) {
    domains.resize(first);
    return false;
  }
  HideRule rule;
  rule.selector = selector;
  rule.exception = exception;
  rule.domains_begin = static_cast<std::uint32_t>(first);
  rule.domains_count = static_cast<std::uint16_t>(domains.size() - first);
  const auto id = static_cast<std::uint32_t>(hides.size());
  hides.push_back(rule);
  bool indexed = false;
  for (const DomainEntry& e : EntriesOf(rule.domains_begin, rule.domains_count)) {
    if (e.exclude) {
      continue;
    }
    std::string_view key = e.name;
    if (HasUpper(key)) {
      owned.push_back(ToLower(key));
      key = owned.back();
    }
    entity_domains = entity_domains || key.ends_with(".*");
    std::vector<std::uint32_t>& list = hides_by_domain[key];
    if (list.empty() || list.back() != id) {
      list.push_back(id);
    }
    indexed = true;
  }
  if (!indexed) {
    hides_everywhere_but.push_back(id);
  }
  return true;
}

void Engine::Impl::Build(const std::vector<std::pair<std::uint32_t, Kind>>& added) {
  // Each rule without a whole host to be found by is filed under its rarest
  // token, so that few rules are tried for any one address.
  std::unordered_map<std::uint64_t, std::uint32_t> frequency;
  std::vector<std::vector<Token>> tokens(rules.size());
  for (const auto& [id, kind] : added) {
    if (HostKey(rules[id]).empty()) {
      tokens[id] = TokensOf(rules[id]);
      for (const Token& t : tokens[id]) {
        ++frequency[t.hash];
      }
    }
  }
  auto file = [&](Index& index, std::uint32_t id) {
    const std::string_view host = HostKey(rules[id]);
    if (!host.empty()) {
      index.by_host[host].push_back(id);
      return;
    }
    const Token* best = nullptr;
    for (const Token& t : tokens[id]) {
      if (!best || frequency[t.hash] < frequency[best->hash] ||
          (frequency[t.hash] == frequency[best->hash] && t.length > best->length)) {
        best = &t;
      }
    }
    if (best) {
      index.by_token[best->hash].push_back(id);
    } else {
      index.rest.push_back(id);
    }
  };
  for (const auto& [id, kind] : added) {
    const NetRule& r = rules[id];
    if (kind == Kind::kBlock) {
      file(blocks, id);
    } else {
      if (r.types) {
        file(exceptions, id);
      }
      if (r.page) {
        file(pages, id);
      }
    }
  }
  for (std::string_view selector : generic) {
    if (!generic_exceptions.count(selector)) {
      AppendHide(&generic_css, selector);
    }
  }
}

const NetRule* Engine::Impl::Find(const Index& index, const RequestContext& ctx,
                                  std::uint32_t type, bool skip_generic,
                                  bool important_only) const {
  const NetRule* found = nullptr;
  index.Any(*ctx.url, [&](std::uint32_t id, std::size_t anchor) {
    const NetRule& r = rules[id];
    if (!(r.types & type) || (important_only && !(r.flags & kImportant))) {
      return false;
    }
    const std::span<const DomainEntry> entries = EntriesOf(r.domains_begin, r.domains_count);
    if ((skip_generic && !HasInclude(entries)) || !MatchRule(r, *ctx.url, anchor)) {
      return false;
    }
    if ((r.party == kThirdParty && !ctx.ThirdParty()) ||
        (r.party == kFirstParty && ctx.ThirdParty()) || !DomainsApply(entries, ctx.host)) {
      return false;
    }
    found = &r;
    return true;
  });
  return found;
}

PageOptions Engine::Impl::Page(std::string_view page_url, std::string_view context_url) const {
  PageOptions out;
  if (pages.by_host.empty() && pages.by_token.empty() && pages.rest.empty()) {
    return out;
  }
  Url page;
  if (!ParseUrl(page_url, &page)) {
    return out;
  }
  Url context;
  const bool has_context = !context_url.empty() && ParseUrl(context_url, &context);
  const HostInfo host(has_context ? context.host() : page.host());
  RequestContext ctx;
  ctx.url = &page;
  ctx.host = &host;
  std::uint8_t flags = 0;
  pages.Any(page, [&](std::uint32_t id, std::size_t anchor) {
    const NetRule& r = rules[id];
    if ((r.page & ~flags) == 0 || !MatchRule(r, page, anchor)) {
      return false;
    }
    if ((r.party == kThirdParty && !ctx.ThirdParty()) ||
        (r.party == kFirstParty && ctx.ThirdParty()) ||
        !DomainsApply(EntriesOf(r.domains_begin, r.domains_count), &host)) {
      return false;
    }
    flags |= r.page;
    return (flags & kPageDocument) != 0;
  });
  out.disabled = (flags & kPageDocument) != 0;
  out.no_hiding = (flags & kPageElemHide) != 0;
  out.no_generic_hiding = (flags & kPageGenericHide) != 0;
  out.no_generic_blocking = (flags & kPageGenericBlock) != 0;
  return out;
}

Engine::Engine(std::span<const std::string_view> rules) : impl_(std::make_unique<Impl>()) {
  std::vector<std::pair<std::uint32_t, Kind>> added;
  for (std::string_view text : rules) {
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
      text.remove_suffix(1);
    }
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
      text.remove_prefix(1);
    }
    if (text.empty() || text.front() == '!' || text.front() == '[') {
      continue;
    }
    if (text.find("##") != npos || text.find("#@#") != npos || text.find("#?#") != npos ||
        text.find("#$#") != npos || text.find("#@?#") != npos || text.find("#@$#") != npos ||
        text.find("#%#") != npos) {
      if (text.find("#?#") == npos && text.find("#$#") == npos && text.find("#@?#") == npos &&
          text.find("#@$#") == npos && text.find("#%#") == npos && impl_->AddHiding(text)) {
        ++impl_->hiding_rules;
      } else {
        ++impl_->ignored;
      }
      continue;
    }
    NetRule r;
    const Kind kind = ParseRequestRule(text, &r, &impl_->domains);
    if (kind == Kind::kIgnored) {
      ++impl_->ignored;
      continue;
    }
    added.emplace_back(static_cast<std::uint32_t>(impl_->rules.size()), kind);
    impl_->rules.push_back(r);
    ++impl_->request_rules;
  }
  impl_->Build(added);
}

Engine::~Engine() = default;

Verdict Engine::Check(const Request& request) const {
  Url url;
  if (!ParseUrl(request.url, &url)) {
    return {};
  }
  const std::string_view frame_url = request.frame_url.empty() ? request.top_url : request.frame_url;
  Url frame;
  const bool has_frame = !frame_url.empty() && ParseUrl(frame_url, &frame);
  const HostInfo host(has_frame ? frame.host() : std::string_view());
  RequestContext ctx;
  ctx.url = &url;
  ctx.host = has_frame ? &host : nullptr;

  PageOptions page = impl_->Page(frame_url, request.frame_url.empty() ? std::string_view()
                                                                      : request.top_url);
  if (!request.top_url.empty() && request.top_url != frame_url) {
    const PageOptions top = impl_->Page(request.top_url, {});
    page.disabled = page.disabled || top.disabled;
    page.no_generic_blocking = page.no_generic_blocking || top.no_generic_blocking;
  }
  if (page.disabled) {
    return {};
  }
  const NetRule* block = impl_->Find(impl_->blocks, ctx, request.type, page.no_generic_blocking, false);
  if (!block) {
    return {};
  }
  if (!(block->flags & kImportant)) {
    if (const NetRule* allow = impl_->Find(impl_->exceptions, ctx, request.type, false, false)) {
      const NetRule* important =
          impl_->Find(impl_->blocks, ctx, request.type, page.no_generic_blocking, true);
      if (!important) {
        return Verdict{false, allow->text};
      }
      block = important;
    }
  }
  return Verdict{true, block->text};
}

PageOptions Engine::ForPage(std::string_view page_url, std::string_view parent_url) const {
  return impl_->Page(page_url, parent_url);
}

std::string Engine::HidingCss(std::string_view raw_host, bool generic) const {
  std::string h = ToLower(raw_host);
  while (!h.empty() && h.back() == '.') {
    h.pop_back();
  }
  const HostInfo info(h);
  std::vector<std::uint32_t> candidates;
  auto look = [&](std::string_view key) {
    const auto it = impl_->hides_by_domain.find(key);
    if (it != impl_->hides_by_domain.end()) {
      candidates.insert(candidates.end(), it->second.begin(), it->second.end());
    }
  };
  if (!h.empty()) {
    for (std::size_t p = 0;;) {
      look(std::string_view(h).substr(p));
      const std::size_t dot = h.find('.', p);
      if (dot == npos) {
        break;
      }
      p = dot + 1;
    }
  }
  if (impl_->entity_domains && !h.empty()) {
    // "example.*" rules, for example.com, www.example.co.uk, ...
    const std::string_view suffix = info.public_suffix();
    if (!suffix.empty() && h.size() > suffix.size() + 1 && h[h.size() - suffix.size() - 1] == '.') {
      const std::string_view stem = std::string_view(h).substr(0, h.size() - suffix.size());
      for (std::size_t p = 0; p + 1 < stem.size();) {
        std::string key(stem.substr(p));
        key += '*';
        look(key);
        const std::size_t dot = stem.find('.', p);
        if (dot == npos || dot + 1 >= stem.size()) {
          break;
        }
        p = dot + 1;
      }
    }
  }
  if (generic) {
    candidates.insert(candidates.end(), impl_->hides_everywhere_but.begin(),
                      impl_->hides_everywhere_but.end());
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

  std::vector<std::string_view> hide;
  std::unordered_set<std::string_view> unhide;
  for (std::uint32_t id : candidates) {
    const HideRule& r = impl_->hides[id];
    if (!DomainsApply(impl_->EntriesOf(r.domains_begin, r.domains_count), &info)) {
      continue;
    }
    if (r.exception) {
      unhide.insert(r.selector);
    } else {
      hide.push_back(r.selector);
    }
  }
  std::string css;
  for (std::string_view selector : hide) {
    if (!unhide.count(selector) && !impl_->generic_exceptions.count(selector)) {
      AppendHide(&css, selector);
    }
  }
  if (generic) {
    if (unhide.empty()) {
      css += impl_->generic_css;
    } else {
      for (std::string_view selector : impl_->generic) {
        if (!impl_->generic_exceptions.count(selector) && !unhide.count(selector)) {
          AppendHide(&css, selector);
        }
      }
    }
  }
  return css;
}

std::size_t Engine::request_rules() const {
  return impl_->request_rules;
}

std::size_t Engine::hiding_rules() const {
  return impl_->hiding_rules;
}

std::size_t Engine::ignored_rules() const {
  return impl_->ignored;
}

std::size_t Engine::memory_bytes() const {
  const Impl& i = *impl_;
  std::size_t n = i.rules.capacity() * sizeof(NetRule) + i.domains.capacity() * sizeof(DomainEntry) +
                  i.blocks.Bytes() + i.exceptions.Bytes() + i.pages.Bytes() +
                  i.hides.capacity() * sizeof(HideRule) + i.generic.capacity() * sizeof(std::string_view) +
                  i.generic_css.capacity() + i.hides_everywhere_but.capacity() * sizeof(std::uint32_t);
  for (const auto& [k, v] : i.hides_by_domain) {
    n += 48 + v.capacity() * sizeof(std::uint32_t);
  }
  for (const std::string& s : i.owned) {
    n += sizeof(std::string) + s.capacity();
  }
  return n;
}

const Engine& BuiltIn() {
  // Never destroyed: request handlers on other threads may use it while
  // the process exits.
  static const Engine* const engine = new Engine(std::span<const std::string_view>(kEasyListRules));
  return *engine;
}

std::string_view BuiltInCommit() {
  return kEasyListCommit;
}

std::size_t BuiltInRuleCount() {
  return std::size(kEasyListRules);
}

}  // namespace torfox::adblock
