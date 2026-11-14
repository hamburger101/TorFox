// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_TOR_SERVICE_H_
#define TORFOX_BROWSER_APP_TOR_SERVICE_H_

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/browser/platform/platform.h"
#include "src/core/tor/bridges.h"

namespace torfox {

namespace tor {
class Controller;
}

// TorFox's own tor: started, configured and controlled here, and stopped
// with TorFox. Web traffic reaches it only through the Tor context's SOCKS
// proxy pref (ContextManager); this class never relays traffic itself.
//
// connected() is the fail-closed switch the Tor windows' request guards
// read on any thread: it is true only while tor reports 100% bootstrap on
// a control connection TorFox authenticated, and the Tor context's proxy
// points at that tor's SOCKS port. It drops to false the moment the control
// connection ends, before anything else happens.
class TorService : public std::enable_shared_from_this<TorService> {
 public:
  enum class State { kOff, kStarting, kConnecting, kConnected, kError };

  struct Status {
    State state = State::kOff;
    int progress = 0;
    std::string message;  // Tor's bootstrap summary, a warning, or the error.
    bool retry = false;   // kError: TorFox will start tor again by itself.
  };

  // Which bridges to use (Settings > Tor).
  struct BridgeConfig {
    std::string mode = "none";  // "none", "builtin", "custom"
    std::string builtin_type = "obfs4";
    std::string custom;
  };

  struct Options {
    std::filesystem::path state_dir;  // <user data>/Tor: torrc, cookie, tor's cache.
    platform::TorPaths paths;
    // Test builds: run this instead of the bundled tor, with these arguments
    // first (a fake tor speaking the control and SOCKS protocols).
    std::filesystem::path test_exe;
    std::vector<std::string> test_args;
    bool notice_log = false;  // Test builds: tor writes notices to state_dir.
  };

  // |apply_port| runs on the UI thread with tor's SOCKS port before the
  // state can become Connected; it must point the Tor context at it and
  // return false if that could not be verified.
  TorService(Options options, std::function<bool(int port)> apply_port);
  ~TorService();

  // UI thread.
  void Start(const BridgeConfig& bridges);
  void Restart(const BridgeConfig& bridges);
  void Shutdown();  // Blocks briefly; tor is stopped when it returns.
  Status status() const;
  int AddObserver(std::function<void()> observer);
  void RemoveObserver(int id);
  // SIGNAL NEWNYM: new circuits for new connections. |done| runs on the UI
  // thread.
  void NewIdentity(std::function<void(bool ok)> done);

  static const char* StateName(State state);
  // tor's own version, once TorFox has taken control of it (empty before).
  std::string version() const;

  // Any thread.
  bool connected() const { return connected_.load(); }

 private:
  void Run(int generation, BridgeConfig bridges);
  bool Session(int generation, const BridgeConfig& bridges);
  void Post(int generation, Status status, int socks_port = 0);
  void OnStatus(int generation, Status status, int socks_port);
  void StopWorker();
  bool Stopping() const { return stop_.load(); }

  const Options options_;
  std::function<bool(int)> apply_port_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<int> generation_{0};
  int port_applied_ = 0;  // UI thread: the port the Tor context uses.
  int restarts_ = 0;
  BridgeConfig bridges_;

  mutable std::mutex lock_;  // Guards status_, alive_, controller_ and writes to connected_.
  Status status_;
  bool alive_ = false;  // An authenticated control connection is open.
  std::string version_;
  std::shared_ptr<tor::Controller> controller_;

  std::map<int, std::function<void()>> observers_;
  int next_observer_ = 1;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_TOR_SERVICE_H_
