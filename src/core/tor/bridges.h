// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_TOR_BRIDGES_H_
#define TORFOX_CORE_TOR_BRIDGES_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Settings > Tor > Bridges: the lines TorFox may write into torrc as
// "Bridge <line>". Every line is checked token by token, so nothing a user
// pastes can add another torrc option, a comment, or a program to run.
namespace torfox::tor {

enum class Transport {
  kVanilla,    // A plain bridge relay: "address:port [fingerprint]".
  kObfs4,      // lyrebird
  kWebTunnel,  // lyrebird
  kSnowflake,  // lyrebird (the bundle's pt_config.json runs snowflake through it)
  kMeekLite,   // lyrebird
};

// "obfs4", "webtunnel", "snowflake", "meek_lite"; "" for kVanilla.
std::string_view TransportName(Transport t);

struct Bridge {
  Transport transport = Transport::kVanilla;
  std::string line;  // Normalised: single spaces, no "Bridge " prefix.
};

constexpr std::size_t kMaxBridgeLineLength = 2048;
constexpr std::size_t kMaxBridges = 64;

// One bridge line as handed out by the Tor Project (bridges.torproject.org,
// or the bundle's pt_config.json):
//   [transport] address:port [fingerprint] [key=value ...]
// An optional leading "Bridge " is accepted. |error| says what is wrong.
std::optional<Bridge> ParseBridgeLine(std::string_view line, std::string* error);

// The custom-bridges setting: one bridge per line; blank lines and lines
// starting with '#' are ignored. Every other line must be valid.
struct BridgeList {
  std::vector<Bridge> bridges;
  std::vector<std::string> errors;  // "Line 3: ..." for each invalid line.
};
BridgeList ParseBridgeLines(std::string_view text);

// True if every line of |text| is a valid bridge line, a comment or blank
// (Settings > Tor > custom bridges; empty text is valid).
bool BridgeTextIsValid(std::string_view text);

// The built-in bridges of |type| ("obfs4", "snowflake" or "meek") listed in
// the Tor Expert Bundle's pt_config.json. Every line is checked as above.
std::vector<Bridge> BuiltinBridges(std::string_view pt_config_json,
                                   std::string_view type,
                                   std::string* error);

}  // namespace torfox::tor

#endif  // TORFOX_CORE_TOR_BRIDGES_H_
