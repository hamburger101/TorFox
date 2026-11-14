// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/tor/controller.h"

#include <utility>

#include "src/core/crypto/os_crypto.h"

namespace torfox::tor {

Controller::Controller(std::unique_ptr<Channel> channel) : channel_(std::move(channel)) {}

Controller::~Controller() {
  Close();
}

void Controller::Close() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    closed_ = true;
  }
  if (channel_) {
    channel_->Close();
  }
}

std::optional<Reply> Controller::ReadReply(std::string* error) {
  for (;;) {
    if (std::optional<Reply> r = reader_.Next()) {
      return r;
    }
    if (reader_.failed()) {
      *error = "tor sent a malformed reply";
      return std::nullopt;
    }
    std::string bytes = channel_->Receive();
    if (bytes.empty()) {
      *error = "the control connection closed";
      return std::nullopt;
    }
    reader_.Feed(bytes);
  }
}

std::optional<Reply> Controller::Call(std::string_view command, std::string* error) {
  std::string line(command);
  line += "\r\n";
  if (!channel_->Send(line)) {
    *error = "could not send to tor";
    return std::nullopt;
  }
  for (;;) {
    std::optional<Reply> r = ReadReply(error);
    if (!r || !r->async()) {
      return r;
    }
    // Events cannot arrive before SETEVENTS; ignore any that do.
  }
}

Controller::SetupResult Controller::Setup(std::string_view cookie) {
  SetupResult result;
  auto fail = [&](std::string why) {
    result.error = std::move(why);
    return result;
  };
  std::string error;
  std::optional<Reply> info = Call("PROTOCOLINFO 1", &error);
  if (!info || !info->ok()) {
    return fail(info ? "tor refused PROTOCOLINFO" : error);
  }
  bool safecookie = false;
  for (const std::string& line : info->lines) {
    if (line.rfind("AUTH ", 0) == 0) {
      std::map<std::string, std::string> kw = ParseKeywords(line.substr(5));
      const std::string methods = "," + kw["METHODS"] + ",";
      safecookie = methods.find(",SAFECOOKIE,") != std::string::npos;
    } else if (line.rfind("VERSION ", 0) == 0) {
      result.tor_version = ParseKeywords(line.substr(8))["Tor"];
    }
  }
  if (!safecookie) {
    return fail("tor does not offer SAFECOOKIE authentication");
  }
  const std::string client_nonce = crypto::RandomBytes(32);
  if (client_nonce.size() != 32) {
    return fail("no random numbers available");
  }
  std::optional<Reply> challenge =
      Call("AUTHCHALLENGE SAFECOOKIE " + crypto::HexEncode(client_nonce), &error);
  if (!challenge || !challenge->ok() || challenge->lines.empty()) {
    return fail(challenge ? "tor refused the authentication challenge" : error);
  }
  std::string text = challenge->lines.front();
  if (text.rfind("AUTHCHALLENGE ", 0) != 0) {
    return fail("unexpected authentication challenge reply");
  }
  std::map<std::string, std::string> kw = ParseKeywords(text.substr(14));
  std::optional<std::string> server_hash = crypto::HexDecode(kw["SERVERHASH"]);
  std::optional<std::string> server_nonce = crypto::HexDecode(kw["SERVERNONCE"]);
  if (!server_hash || !server_nonce) {
    return fail("unexpected authentication challenge reply");
  }
  std::optional<SafeCookie> hashes = SafeCookieHashes(cookie, client_nonce, *server_nonce);
  if (!hashes) {
    return fail("the authentication cookie is not valid");
  }
  if (!crypto::ConstantTimeEqual(*server_hash, hashes->server_hash)) {
    return fail("the control port did not prove it is TorFox's tor");
  }
  std::optional<Reply> auth = Call("AUTHENTICATE " + crypto::HexEncode(hashes->client_hash), &error);
  if (!auth || !auth->ok()) {
    return fail(auth ? "tor refused the authentication" : error);
  }
  for (const char* command : {"TAKEOWNERSHIP", "SETEVENTS STATUS_CLIENT"}) {
    std::optional<Reply> r = Call(command, &error);
    if (!r || !r->ok()) {
      return fail(r ? std::string("tor refused ") + command : error);
    }
  }
  std::optional<Reply> socks = Call("GETINFO net/listeners/socks", &error);
  if (!socks || !socks->ok() || socks->lines.empty()) {
    return fail(socks ? "tor did not report its SOCKS port" : error);
  }
  constexpr std::string_view kSocks = "net/listeners/socks=";
  const std::string& first = socks->lines.front();
  if (first.rfind(kSocks, 0) != 0) {
    return fail("tor did not report its SOCKS port");
  }
  std::optional<int> port = ParseLoopbackListener(std::string_view(first).substr(kSocks.size()));
  if (!port) {
    return fail("tor's SOCKS port is not on 127.0.0.1");
  }
  result.socks_port = *port;
  std::optional<Reply> phase = Call("GETINFO status/bootstrap-phase", &error);
  if (!phase || !phase->ok() || phase->lines.empty()) {
    return fail(phase ? "tor did not report its bootstrap state" : error);
  }
  std::optional<Bootstrap> b = ParseBootstrap(phase->lines.front());
  if (!b) {
    return fail("tor's bootstrap state could not be read");
  }
  result.bootstrap = *b;
  result.ok = true;
  return result;
}

bool Controller::Command(std::string command, std::function<void(const Reply&)> done) {
  if (command.find_first_of("\r\n") != std::string::npos) {
    return false;
  }
  std::lock_guard<std::mutex> guard(lock_);
  if (closed_ || !running_) {
    return false;
  }
  pending_.push_back(std::move(done));
  command += "\r\n";
  if (!channel_->Send(command)) {
    pending_.pop_back();
    return false;
  }
  return true;
}

std::string Controller::Run(const std::function<void(const Reply&)>& on_event) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    running_ = !closed_;
  }
  std::string error;
  while (running_) {
    std::optional<Reply> r = ReadReply(&error);
    if (!r) {
      break;
    }
    if (r->async()) {
      on_event(*r);
      continue;
    }
    std::function<void(const Reply&)> done;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (pending_.empty()) {
        error = "tor sent a reply nobody asked for";
        break;
      }
      done = std::move(pending_.front());
      pending_.pop_front();
    }
    if (done) {
      done(*r);
    }
  }
  std::deque<std::function<void(const Reply&)>> left;
  {
    std::lock_guard<std::mutex> guard(lock_);
    running_ = false;
    left.swap(pending_);
  }
  for (auto& done : left) {
    if (done) {
      done(Reply());
    }
  }
  return error.empty() ? "the control connection closed" : error;
}

}  // namespace torfox::tor
