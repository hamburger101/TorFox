// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A blocking TCP connection to 127.0.0.1, for tor's control port. Nothing
// else in TorFox opens sockets itself; web traffic is Chromium's.

#include <atomic>
#include <cstring>

#include "src/core/tor/controller.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace torfox::tor {
namespace {

void CloseSocket(SocketHandle s) {
#if defined(_WIN32)
  closesocket(s);
#else
  ::close(s);
#endif
}

bool EnsureStarted() {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
#else
  return true;
#endif
}

bool SetBlocking(SocketHandle s, bool blocking) {
#if defined(_WIN32)
  u_long nonblocking = blocking ? 0 : 1;
  return ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
  const int flags = fcntl(s, F_GETFL, 0);
  return flags >= 0 &&
         fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

class LoopbackChannel : public Channel {
 public:
  explicit LoopbackChannel(SocketHandle s) : socket_(s) {}
  ~LoopbackChannel() override {
    Close();
    CloseSocket(socket_);
  }

  bool Send(std::string_view bytes) override {
    while (!bytes.empty()) {
      if (closed_) {
        return false;
      }
#if defined(_WIN32)
      const int n = ::send(socket_, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
#if defined(__APPLE__)
      constexpr int kFlags = 0;  // SO_NOSIGPIPE is set on the socket instead.
#else
      constexpr int kFlags = MSG_NOSIGNAL;
#endif
      const ssize_t n = ::send(socket_, bytes.data(), bytes.size(), kFlags);
#endif
      if (n <= 0) {
        return false;
      }
      bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }

  std::string Receive() override {
    char buf[4096];
    if (closed_) {
      return std::string();
    }
#if defined(_WIN32)
    const int n = ::recv(socket_, buf, sizeof(buf), 0);
#else
    const ssize_t n = ::recv(socket_, buf, sizeof(buf), 0);
#endif
    if (n <= 0) {
      return std::string();
    }
    return std::string(buf, static_cast<std::size_t>(n));
  }

  void Close() override {
    if (!closed_.exchange(true)) {
      // shutdown() wakes a recv() blocked on another thread; the handle
      // itself is closed only in the destructor, after that thread is done.
#if defined(_WIN32)
      ::shutdown(socket_, SD_BOTH);
#else
      ::shutdown(socket_, SHUT_RDWR);
#endif
    }
  }

 private:
  SocketHandle socket_;
  std::atomic<bool> closed_{false};
};

}  // namespace

std::unique_ptr<Channel> ConnectLoopback(int port, int timeout_ms, std::string* error) {
  auto fail = [&](const char* why) -> std::unique_ptr<Channel> {
    if (error) {
      *error = why;
    }
    return nullptr;
  };
  if (port <= 0 || port > 65535) {
    return fail("bad port");
  }
  if (!EnsureStarted()) {
    return fail("sockets are not available");
  }
  SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) {
    return fail("could not create a socket");
  }
#if defined(__APPLE__)
  const int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (!SetBlocking(s, false)) {
    CloseSocket(s);
    return fail("could not configure the socket");
  }
  int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  bool connected = rc == 0;
  if (!connected) {
#if defined(_WIN32)
    const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
    if (pending) {
      fd_set writable, failed;
      FD_ZERO(&writable);
      FD_ZERO(&failed);
      FD_SET(s, &writable);
      FD_SET(s, &failed);
      timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
      connected = ::select(0, nullptr, &writable, &failed, &tv) == 1 && FD_ISSET(s, &writable);
    }
#else
    if (errno == EINPROGRESS) {
      pollfd p{s, POLLOUT, 0};
      if (::poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLOUT)) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        connected = getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0;
      }
    }
#endif
  }
  if (!connected || !SetBlocking(s, true)) {
    CloseSocket(s);
    return fail("could not connect to tor's control port");
  }
  return std::make_unique<LoopbackChannel>(s);
}

}  // namespace torfox::tor
