// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_TOR_TORRC_H_
#define TORFOX_CORE_TOR_TORRC_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/tor/bridges.h"

namespace torfox::tor {

// What TorFox's tor needs to know. tor runs with its working directory set
// to the folder that holds tor and pluggable_transports/, because
// ClientTransportPlugin cannot quote a path (it is split at spaces).
struct TorrcOptions {
  std::filesystem::path data_dir;           // DataDirectory; also holds the cookie.
  std::filesystem::path control_port_file;  // Where tor writes "PORT=127.0.0.1:<n>".
  std::filesystem::path geoip;              // Optional.
  std::filesystem::path geoip6;             // Optional.
  // lyrebird, relative to tor's working directory ("pluggable_transports/lyrebird.exe").
  // Empty: bridges that need a transport are refused.
  std::string pt_client;
  long owning_pid = 0;                      // tor exits when this process does.
  std::vector<Bridge> bridges;              // Empty: no bridges.
  std::filesystem::path notice_log;         // Optional (test builds): "Log notice file ...".
};

// The torrc text, or "" with |error| set when a value cannot be written
// safely (a path with characters torrc cannot carry, a missing transport).
//
// Always: SOCKS and control ports on 127.0.0.1 only, picked by tor ("auto");
// cookie authentication; ClientOnly; tor exits with TorFox
// (__OwningControllerProcess, and the control connection takes ownership);
// IsolateDestAddr (a separate circuit per destination; Chromium's SOCKS
// client sends no credentials, so per-tab isolation is not possible);
// ExtendedErrors (onion-service failures get their own SOCKS replies).
std::string BuildTorrc(const TorrcOptions& options, std::string* error);

// A path as a quoted torrc value, or "" if it cannot be written (empty,
// control characters, or non-ASCII; the browser layer passes a short path
// for profiles under non-ASCII folders on Windows).
std::string QuotePath(const std::filesystem::path& path);

}  // namespace torfox::tor

#endif  // TORFOX_CORE_TOR_TORRC_H_
