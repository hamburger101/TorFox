// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/tor/control.h"

#include <charconv>

#include "src/core/crypto/os_crypto.h"

namespace torfox::tor {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

std::optional<int> ParseInt(std::string_view s, int lo, int hi) {
  if (s.empty() || s.size() > 6) {
    return std::nullopt;
  }
  for (char c : s) {
    if (!IsDigit(c)) {
      return std::nullopt;
    }
  }
  int v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  if (v < lo || v > hi) {
    return std::nullopt;
  }
  return v;
}

std::optional<int> LoopbackPort(std::string_view addr) {
  constexpr std::string_view kPrefix = "127.0.0.1:";
  if (addr.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  return ParseInt(addr.substr(kPrefix.size()), 1, 65535);
}

}  // namespace

void ReplyReader::Feed(std::string_view bytes) {
  if (failed_) {
    return;
  }
  buffer_.append(bytes);
  std::size_t start = 0;
  while (!failed_) {
    const std::size_t nl = buffer_.find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    std::string line = buffer_.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    start = nl + 1;
    if (line.size() > kMaxLine) {
      failed_ = true;
      break;
    }
    Line(std::move(line));
  }
  buffer_.erase(0, start);
  if (buffer_.size() > kMaxLine) {
    failed_ = true;
  }
}

void ReplyReader::Line(std::string line) {
  current_size_ += line.size() + 2;
  if (current_size_ > kMaxReply) {
    failed_ = true;
    return;
  }
  if (in_data_) {
    if (line == ".") {
      in_data_ = false;
      current_.lines.push_back(std::move(data_));
      data_.clear();
      return;
    }
    if (!line.empty() && line[0] == '.') {
      line.erase(0, 1);
    }
    data_ += line;
    data_ += '\n';
    return;
  }
  if (line.size() < 4 || !IsDigit(line[0]) || !IsDigit(line[1]) || !IsDigit(line[2]) ||
      (line[3] != ' ' && line[3] != '-' && line[3] != '+')) {
    failed_ = true;
    return;
  }
  const int status = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
  if (current_.status != 0 && current_.status != status) {
    failed_ = true;
    return;
  }
  current_.status = status;
  const char sep = line[3];
  std::string text = line.substr(4);
  if (sep == '+') {
    in_data_ = true;
    data_ = text + "\n";
    return;
  }
  current_.lines.push_back(std::move(text));
  if (sep == ' ') {
    ready_.push_back(std::move(current_));
    current_ = Reply();
    current_size_ = 0;
  }
}

std::optional<Reply> ReplyReader::Next() {
  if (ready_.empty()) {
    return std::nullopt;
  }
  Reply r = std::move(ready_.front());
  ready_.pop_front();
  return r;
}

std::optional<std::string> UnquoteString(std::string_view text, std::size_t* consumed) {
  if (text.empty() || text[0] != '"') {
    return std::nullopt;
  }
  std::string out;
  for (std::size_t i = 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') {
      if (consumed) {
        *consumed = i + 1;
      }
      return out;
    }
    if (c == '\\' && i + 1 < text.size()) {
      const char e = text[++i];
      switch (e) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        default:
          out.push_back(e);  // \\ \" and anything else as itself.
      }
      continue;
    }
    out.push_back(c);
  }
  return std::nullopt;
}

std::string QuoteString(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::map<std::string, std::string> ParseKeywords(std::string_view text) {
  std::map<std::string, std::string> out;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    std::size_t key_end = i;
    while (key_end < text.size() && text[key_end] != ' ' && text[key_end] != '=') {
      ++key_end;
    }
    const std::string key(text.substr(i, key_end - i));
    if (key_end < text.size() && text[key_end] == '=') {
      std::size_t v = key_end + 1;
      if (v < text.size() && text[v] == '"') {
        std::size_t used = 0;
        std::optional<std::string> value = UnquoteString(text.substr(v), &used);
        if (!value) {
          break;  // Unterminated quote: stop rather than guess.
        }
        out[key] = *value;
        i = v + used;
      } else {
        std::size_t end = text.find(' ', v);
        if (end == std::string_view::npos) {
          end = text.size();
        }
        out[key] = std::string(text.substr(v, end - v));
        i = end;
      }
    } else {
      out.emplace(key, std::string());
      i = key_end;
    }
  }
  return out;
}

std::optional<Bootstrap> ParseBootstrap(std::string_view text) {
  // Drop the event name ("STATUS_CLIENT ") or key ("status/bootstrap-phase=").
  if (text.substr(0, 14) == "STATUS_CLIENT ") {
    text.remove_prefix(14);
  }
  constexpr std::string_view kKey = "status/bootstrap-phase=";
  if (text.substr(0, kKey.size()) == kKey) {
    text.remove_prefix(kKey.size());
  }
  const std::size_t space = text.find(' ');
  if (space == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view severity = text.substr(0, space);
  std::map<std::string, std::string> kw = ParseKeywords(text.substr(space + 1));
  if (!kw.count("BOOTSTRAP") || !kw.count("PROGRESS")) {
    return std::nullopt;
  }
  std::optional<int> progress = ParseInt(kw["PROGRESS"], 0, 100);
  if (!progress) {
    return std::nullopt;
  }
  Bootstrap b;
  b.progress = *progress;
  b.tag = kw["TAG"];
  b.summary = kw["SUMMARY"];
  if (severity == "WARN" || severity == "ERR") {
    b.warning = kw.count("WARNING") && !kw["WARNING"].empty() ? kw["WARNING"] : b.summary;
    if (b.warning.empty()) {
      b.warning = "Tor reported a problem while connecting.";
    }
  }
  return b;
}

std::optional<int> ParseControlPortFile(std::string_view content) {
  std::size_t start = 0;
  while (start < content.size()) {
    std::size_t end = content.find('\n', start);
    if (end == std::string_view::npos) {
      end = content.size();
    }
    std::string_view line = content.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.substr(0, 5) == "PORT=") {
      return LoopbackPort(line.substr(5));
    }
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<int> ParseLoopbackListener(std::string_view value) {
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && value[i] == ' ') {
      ++i;
    }
    if (i >= value.size()) {
      break;
    }
    std::string token;
    if (value[i] == '"') {
      std::size_t used = 0;
      std::optional<std::string> q = UnquoteString(value.substr(i), &used);
      if (!q) {
        return std::nullopt;
      }
      token = *q;
      i += used;
    } else {
      std::size_t end = value.find(' ', i);
      if (end == std::string_view::npos) {
        end = value.size();
      }
      token = std::string(value.substr(i, end - i));
      i = end;
    }
    if (std::optional<int> port = LoopbackPort(token)) {
      return port;
    }
  }
  return std::nullopt;
}

std::optional<SafeCookie> SafeCookieHashes(std::string_view cookie,
                                           std::string_view client_nonce,
                                           std::string_view server_nonce) {
  if (cookie.size() != 32 || client_nonce.size() != 32 || server_nonce.size() != 32) {
    return std::nullopt;
  }
  std::string message;
  message.append(cookie).append(client_nonce).append(server_nonce);
  auto server = crypto::HmacSha256(
      "Tor safe cookie authentication server-to-controller hash", message);
  auto client = crypto::HmacSha256(
      "Tor safe cookie authentication controller-to-server hash", message);
  if (!server || !client) {
    return std::nullopt;
  }
  SafeCookie out;
  out.server_hash.assign(reinterpret_cast<const char*>(server->data()), server->size());
  out.client_hash.assign(reinterpret_cast<const char*>(client->data()), client->size());
  return out;
}

}  // namespace torfox::tor
