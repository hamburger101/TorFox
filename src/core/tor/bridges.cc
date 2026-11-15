// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/tor/bridges.h"

#include <charconv>

#include "src/core/json.h"

namespace torfox::tor {
namespace {

std::vector<std::string_view> SplitTokens(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
      ++i;
    }
    if (i > start) {
      out.push_back(s.substr(start, i - start));
    }
  }
  return out;
}

bool AllOf(std::string_view s, bool (*pred)(char)) {
  for (char c : s) {
    if (!pred(c)) {
      return false;
    }
  }
  return !s.empty();
}

bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}
// Values: printable ASCII without space, quotes, backslash or '#' (torrc
// treats '#' as the start of a comment and '"' / '\' as quoting).
bool IsValueChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u > 0x20 && u < 0x7F && c != '"' && c != '\\' && c != '#' && c != '\'';
}

bool ParsePort(std::string_view s) {
  if (s.empty() || s.size() > 5 || !AllOf(s, IsDigit)) {
    return false;
  }
  int port = 0;
  std::from_chars(s.data(), s.data() + s.size(), port);
  return port >= 1 && port <= 65535;
}

bool ParseIpv4(std::string_view s) {
  int parts = 0;
  std::size_t i = 0;
  while (i <= s.size()) {
    const std::size_t dot = s.find('.', i);
    const std::string_view part = s.substr(i, dot == std::string_view::npos ? s.npos : dot - i);
    if (part.empty() || part.size() > 3 || !AllOf(part, IsDigit) ||
        (part.size() > 1 && part[0] == '0')) {
      return false;
    }
    int v = 0;
    std::from_chars(part.data(), part.data() + part.size(), v);
    if (v > 255) {
      return false;
    }
    ++parts;
    if (dot == std::string_view::npos) {
      break;
    }
    i = dot + 1;
  }
  return parts == 4;
}

bool ParseIpv6(std::string_view s) {
  // Hex groups and colons, at most one "::", 2..39 characters. Tor parses
  // the address itself; this only keeps the token to address characters.
  if (s.size() < 2 || s.size() > 39) {
    return false;
  }
  int groups = 0;
  bool compressed = false;
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == ':') {
      if (i + 1 < s.size() && s[i + 1] == ':') {
        if (compressed) {
          return false;
        }
        compressed = true;
        i += 2;
        continue;
      }
      if (i == 0 || i + 1 == s.size() || !IsHex(s[i - 1])) {
        return false;  // A single ':' sits between two groups.
      }
      ++i;
      continue;
    }
    std::size_t len = 0;
    while (i + len < s.size() && IsHex(s[i + len])) {
      ++len;
    }
    if (len == 0 || len > 4) {
      return false;
    }
    ++groups;
    i += len;
  }
  return compressed ? groups <= 7 : groups == 8;
}

bool ParseAddress(std::string_view s) {
  if (!s.empty() && s.front() == '[') {
    const std::size_t close = s.find(']');
    if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != ':') {
      return false;
    }
    return ParseIpv6(s.substr(1, close - 1)) && ParsePort(s.substr(close + 2));
  }
  const std::size_t colon = s.rfind(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  return ParseIpv4(s.substr(0, colon)) && ParsePort(s.substr(colon + 1));
}

std::optional<Transport> TransportFromName(std::string_view name) {
  if (name == "obfs4") return Transport::kObfs4;
  if (name == "webtunnel") return Transport::kWebTunnel;
  if (name == "snowflake") return Transport::kSnowflake;
  if (name == "meek_lite") return Transport::kMeekLite;
  return std::nullopt;
}

}  // namespace

std::string_view TransportName(Transport t) {
  switch (t) {
    case Transport::kObfs4:
      return "obfs4";
    case Transport::kWebTunnel:
      return "webtunnel";
    case Transport::kSnowflake:
      return "snowflake";
    case Transport::kMeekLite:
      return "meek_lite";
    case Transport::kVanilla:
      break;
  }
  return "";
}

std::optional<Bridge> ParseBridgeLine(std::string_view line, std::string* error) {
  auto fail = [&](const char* why) -> std::optional<Bridge> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };
  if (line.size() > kMaxBridgeLineLength) {
    return fail("the line is too long");
  }
  for (char c : line) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u < 0x20 && c != '\t') || u >= 0x7F) {
      return fail("the line contains characters a bridge line never has");
    }
  }
  std::vector<std::string_view> tokens = SplitTokens(line);
  if (!tokens.empty() && tokens.front() == "Bridge") {
    tokens.erase(tokens.begin());
  }
  if (tokens.empty()) {
    return fail("the line is empty");
  }
  Bridge bridge;
  std::size_t next = 0;
  if (!tokens[0].empty() && (IsDigit(tokens[0][0]) || tokens[0][0] == '[')) {
    bridge.transport = Transport::kVanilla;
  } else {
    std::optional<Transport> t = TransportFromName(tokens[0]);
    if (!t) {
      return fail("TorFox supports obfs4, WebTunnel, Snowflake and meek bridges, and plain bridge relays");
    }
    bridge.transport = *t;
    next = 1;
  }
  if (next >= tokens.size() || !ParseAddress(tokens[next])) {
    return fail("the address must be an IP address and port, such as 192.0.2.1:443");
  }
  ++next;
  if (next < tokens.size() && tokens[next].find('=') == std::string_view::npos) {
    if (tokens[next].size() != 40 || !AllOf(tokens[next], IsHex)) {
      return fail("the fingerprint must be 40 hexadecimal characters");
    }
    ++next;
  }
  for (; next < tokens.size(); ++next) {
    if (bridge.transport == Transport::kVanilla) {
      return fail("a plain bridge relay takes only an address and a fingerprint");
    }
    const std::string_view arg = tokens[next];
    const std::size_t eq = arg.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == arg.size() ||
        !AllOf(arg.substr(0, eq), IsKeyChar) || !AllOf(arg.substr(eq + 1), IsValueChar)) {
      return fail("transport options must look like key=value, without spaces or quotes");
    }
  }
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      bridge.line.push_back(' ');
    }
    bridge.line.append(tokens[i]);
  }
  return bridge;
}

BridgeList ParseBridgeLines(std::string_view text) {
  BridgeList out;
  std::size_t start = 0;
  int number = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    ++number;
    std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos && line[first] != '#') {
      std::string error;
      if (std::optional<Bridge> b = ParseBridgeLine(line, &error)) {
        if (out.bridges.size() >= kMaxBridges) {
          out.errors.push_back("Line " + std::to_string(number) + ": more than " +
                               std::to_string(kMaxBridges) + " bridges");
        } else {
          out.bridges.push_back(std::move(*b));
        }
      } else {
        out.errors.push_back("Line " + std::to_string(number) + ": " + error);
      }
    }
    if (end == text.size()) {
      break;
    }
    start = end + 1;
  }
  return out;
}

bool BridgeTextIsValid(std::string_view text) {
  return ParseBridgeLines(text).errors.empty();
}

std::vector<Bridge> BuiltinBridges(std::string_view pt_config_json,
                                   std::string_view type,
                                   std::string* error) {
  std::vector<Bridge> out;
  auto fail = [&](std::string why) {
    if (error) {
      *error = std::move(why);
    }
    return std::vector<Bridge>();
  };
  std::optional<Json> config = ParseJson(pt_config_json, 1 << 20, 16);
  if (!config) {
    return fail("pt_config.json could not be read");
  }
  const Json* bridges = GetMember(*config, "bridges");
  const Json* list = bridges ? GetMember(*bridges, type) : nullptr;
  if (!list || !list->is_array() || list->empty()) {
    return fail("the Tor bundle lists no built-in " + std::string(type) + " bridges");
  }
  for (const Json& entry : *list) {
    if (!entry.is_string()) {
      return fail("pt_config.json has a bridge entry that is not text");
    }
    std::string why;
    std::optional<Bridge> b = ParseBridgeLine(entry.get_ref<const std::string&>(), &why);
    if (!b) {
      return fail("a built-in bridge line is invalid: " + why);
    }
    out.push_back(std::move(*b));
    if (out.size() >= kMaxBridges) {
      break;
    }
  }
  return out;
}

}  // namespace torfox::tor
