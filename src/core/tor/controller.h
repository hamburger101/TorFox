// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_TOR_CONTROLLER_H_
#define TORFOX_CORE_TOR_CONTROLLER_H_

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "src/core/tor/control.h"

namespace torfox::tor {

// A byte stream to tor's control port.
class Channel {
 public:
  virtual ~Channel() = default;
  virtual bool Send(std::string_view bytes) = 0;
  // Blocks until bytes arrive. "" means the connection closed or failed.
  virtual std::string Receive() = 0;
  // Makes a blocked Receive() return "" (any thread).
  virtual void Close() = 0;
};

// A connection to 127.0.0.1:|port|, or null with |error|.
std::unique_ptr<Channel> ConnectLoopback(int port, int timeout_ms, std::string* error);

// The controlling connection to TorFox's own tor.
//
// Setup() runs on the thread that will then call Run(); while Run() reads
// replies and events, Command() may be called from any thread. Replies are
// matched to commands in order (control-spec 2.1).
class Controller {
 public:
  explicit Controller(std::unique_ptr<Channel> channel);
  ~Controller();

  struct SetupResult {
    bool ok = false;
    std::string error;
    std::string tor_version;
    int socks_port = 0;
    Bootstrap bootstrap;
  };
  // SAFECOOKIE authentication with |cookie| (the 32 bytes tor wrote), then
  // TAKEOWNERSHIP (tor exits when this connection closes), SETEVENTS
  // STATUS_CLIENT, and the SOCKS port and bootstrap state. The server's
  // proof is checked before TorFox reveals anything derived from the cookie,
  // so a program that is not tor learns nothing from the attempt.
  SetupResult Setup(std::string_view cookie);

  // Reads until the connection ends; |on_event| gets each 650 event.
  // Returns why it ended.
  std::string Run(const std::function<void(const Reply&)>& on_event);

  // Sends |command| (no CRLF); |done| runs on the Run() thread with the
  // reply, or with status 0 if the connection ends first. False if it could
  // not be sent.
  bool Command(std::string command, std::function<void(const Reply&)> done);

  void Close();

 private:
  std::optional<Reply> Call(std::string_view command, std::string* error);
  std::optional<Reply> ReadReply(std::string* error);

  std::unique_ptr<Channel> channel_;
  ReplyReader reader_;
  std::mutex lock_;
  std::deque<std::function<void(const Reply&)>> pending_;
  bool running_ = false;
  bool closed_ = false;
};

}  // namespace torfox::tor

#endif  // TORFOX_CORE_TOR_CONTROLLER_H_
