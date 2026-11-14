// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_TOR_CONTROL_H_
#define TORFOX_CORE_TOR_CONTROL_H_

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tor's control protocol (control-spec.txt): reading replies, and the
// values TorFox takes out of them. No I/O here.
namespace torfox::tor {

struct Reply {
  int status = 0;                  // 250, 510, 650, ...
  std::vector<std::string> lines;  // Text after "NNN-", "NNN+" (with its data) or "NNN ".
  bool ok() const { return status == 250; }
  bool async() const { return status >= 600 && status < 700; }
};

// Splits a byte stream into replies. Lines end in CRLF (a bare LF is
// accepted). "NNN+" starts a data block that ends with a line holding only
// "."; leading dots are unescaped. A line over 64 KiB, a reply over 1 MiB, a
// malformed status line, or a status that changes inside a reply makes the
// reader fail for good: the connection cannot be trusted after that.
class ReplyReader {
 public:
  static constexpr std::size_t kMaxLine = 64 * 1024;
  static constexpr std::size_t kMaxReply = 1024 * 1024;

  void Feed(std::string_view bytes);
  std::optional<Reply> Next();
  bool failed() const { return failed_; }

 private:
  void Line(std::string line);

  std::string buffer_;
  Reply current_;
  std::size_t current_size_ = 0;
  bool in_data_ = false;
  std::string data_;
  std::deque<Reply> ready_;
  bool failed_ = false;
};

// Parses "KEY=VALUE KEY2=\"quoted value\" FLAG" (control-spec 2.1.1 and
// QuotedString escapes). Bare words are returned with an empty value under
// their own name. Keys are case-sensitive.
std::map<std::string, std::string> ParseKeywords(std::string_view text);

// Unescapes a QuotedString starting at |text|[0] == '"'; |consumed| gets the
// length used. nullopt if the quote is not closed.
std::optional<std::string> UnquoteString(std::string_view text, std::size_t* consumed);

// A command argument as a QuotedString.
std::string QuoteString(std::string_view value);

// Bootstrap status, from "NOTICE BOOTSTRAP PROGRESS=50 TAG=... SUMMARY=..."
// (GETINFO status/bootstrap-phase) or the STATUS_CLIENT event of that form.
// A WARN severity with WARNING=... is a problem tor will keep retrying.
struct Bootstrap {
  int progress = 0;  // 0..100
  std::string tag;
  std::string summary;
  std::string warning;  // Non-empty for a bootstrap problem.
};
std::optional<Bootstrap> ParseBootstrap(std::string_view text);

// The control port from tor's ControlPortWriteToFile file ("PORT=127.0.0.1:N").
// Only a 127.0.0.1 address is accepted.
std::optional<int> ParseControlPortFile(std::string_view content);

// The first 127.0.0.1 port in a GETINFO net/listeners/socks value
// ("\"127.0.0.1:9150\" ...").
std::optional<int> ParseLoopbackListener(std::string_view value);

// SAFECOOKIE authentication (control-spec 3.24). The hashes are
// HMAC-SHA256(key, cookie | client_nonce | server_nonce).
struct SafeCookie {
  std::string server_hash;  // What tor must have sent.
  std::string client_hash;  // What TorFox sends back.
};
std::optional<SafeCookie> SafeCookieHashes(std::string_view cookie,
                                           std::string_view client_nonce,
                                           std::string_view server_nonce);

}  // namespace torfox::tor

#endif  // TORFOX_CORE_TOR_CONTROL_H_
