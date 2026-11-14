// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/tor/torrc.h"

namespace torfox::tor {
namespace {

bool SafeRelativeProgram(std::string_view p) {
  if (p.empty() || p.size() > 200 || p.front() == '/' || p.front() == '\\' ||
      p.find("..") != std::string_view::npos || p.find(':') != std::string_view::npos) {
    return false;
  }
  for (char c : p) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.' || c == '/' || c == '\\';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string QuotePath(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  if (u8.empty()) {
    return std::string();
  }
  std::string out = "\"";
  for (char8_t ch : u8) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (u < 0x20 || u >= 0x7F) {
      return std::string();
    }
    if (ch == u8'\\' || ch == u8'"') {
      out.push_back('\\');
    }
    out.push_back(static_cast<char>(ch));
  }
  out.push_back('"');
  return out;
}

std::string BuildTorrc(const TorrcOptions& o, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) {
      *error = why;
    }
    return std::string();
  };
  const std::string data_dir = QuotePath(o.data_dir);
  const std::string cookie = QuotePath(o.data_dir / "control_auth_cookie");
  const std::string port_file = QuotePath(o.control_port_file);
  if (data_dir.empty() || cookie.empty() || port_file.empty()) {
    return fail("TorFox's profile folder has a name tor cannot use");
  }
  if (o.owning_pid <= 0) {
    return fail("no owning process");
  }
  std::string t;
  t += "# Written by TorFox each time it starts tor. Changes here are overwritten.\n";
  t += "DataDirectory " + data_dir + "\n";
  t += "SocksPort 127.0.0.1:auto ExtendedErrors IsolateDestAddr\n";
  t += "ControlPort 127.0.0.1:auto\n";
  t += "ControlPortWriteToFile " + port_file + "\n";
  t += "CookieAuthentication 1\n";
  t += "CookieAuthFile " + cookie + "\n";
  t += "__OwningControllerProcess " + std::to_string(o.owning_pid) + "\n";
  t += "ClientOnly 1\n";
  t += "AvoidDiskWrites 1\n";
  t += "SafeLogging 1\n";
  t += "DormantCanceledByStartup 1\n";
  if (!o.notice_log.empty()) {
    const std::string log = QuotePath(o.notice_log);
    if (log.empty()) {
      return fail("the log path cannot be used");
    }
    // Log's value is "severity file path", and tor unquotes a value only
    // when the whole of it is quoted: "Log notice file "C:\\x"" would make
    // tor look for a file whose name has quotes in it (and refuse to start).
    t += "Log \"notice file " + log.substr(1) + "\n";
  } else {
    t += "Log notice stdout\n";
  }
  for (const auto& [option, path] : {std::pair{"GeoIPFile", &o.geoip},
                                     std::pair{"GeoIPv6File", &o.geoip6}}) {
    if (!path->empty()) {
      const std::string q = QuotePath(*path);
      if (q.empty()) {
        return fail("the GeoIP path cannot be used");
      }
      t += std::string(option) + " " + q + "\n";
    }
  }
  if (!o.bridges.empty()) {
    bool needs_pt = false;
    for (const Bridge& b : o.bridges) {
      needs_pt = needs_pt || b.transport != Transport::kVanilla;
    }
    if (needs_pt) {
      if (!SafeRelativeProgram(o.pt_client)) {
        return fail("the bridge transport program is missing");
      }
      t += "ClientTransportPlugin meek_lite,obfs4,webtunnel exec " + o.pt_client + "\n";
      t += "ClientTransportPlugin snowflake exec " + o.pt_client + "\n";
    }
    t += "UseBridges 1\n";
    for (const Bridge& b : o.bridges) {
      // Checked again here: nothing else may ever reach torrc.
      std::string why;
      std::optional<Bridge> again = ParseBridgeLine(b.line, &why);
      if (!again || again->line != b.line) {
        return fail("a bridge line is invalid");
      }
      t += "Bridge " + b.line + "\n";
    }
  }
  return t;
}

}  // namespace torfox::tor
