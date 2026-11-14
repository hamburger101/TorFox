// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/tor_service.h"

#include <chrono>
#include <optional>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/test_log.h"
#include "src/core/tor/controller.h"
#include "src/core/tor/torrc.h"
#include "src/core/util/file_io.h"

namespace torfox {
namespace {

constexpr int kStartTimeoutMs = 60 * 1000;
constexpr int kMaxAutomaticRestarts = 3;
constexpr int kRestartDelayMs[kMaxAutomaticRestarts] = {2000, 5000, 15000};

void Sleep(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

std::string TorService::version() const {
  std::lock_guard<std::mutex> guard(lock_);
  return version_;
}

const char* TorService::StateName(State state) {
  switch (state) {
    case State::kOff:
      return "off";
    case State::kStarting:
      return "starting";
    case State::kConnecting:
      return "connecting";
    case State::kConnected:
      return "connected";
    case State::kError:
      return "error";
  }
  return "error";
}

TorService::TorService(Options options, std::function<bool(int port)> apply_port)
    : options_(std::move(options)), apply_port_(std::move(apply_port)) {}

TorService::~TorService() {
  StopWorker();
}

TorService::Status TorService::status() const {
  std::lock_guard<std::mutex> guard(lock_);
  return status_;
}

int TorService::AddObserver(std::function<void()> observer) {
  CEF_REQUIRE_UI_THREAD();
  const int id = next_observer_++;
  observers_[id] = std::move(observer);
  return id;
}

void TorService::RemoveObserver(int id) {
  CEF_REQUIRE_UI_THREAD();
  observers_.erase(id);
}

void TorService::Start(const BridgeConfig& bridges) {
  CEF_REQUIRE_UI_THREAD();
  const State state = status().state;
  if (state == State::kStarting || state == State::kConnecting || state == State::kConnected) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();  // A finished session (it ended in an error).
  }
  bridges_ = bridges;
  const int generation = ++generation_;
  {
    std::lock_guard<std::mutex> guard(lock_);
    status_ = Status{State::kStarting, 0, "Starting Tor"};
  }
  worker_ = std::thread([this, generation, bridges] { Run(generation, bridges); });
}

void TorService::Restart(const BridgeConfig& bridges) {
  CEF_REQUIRE_UI_THREAD();
  StopWorker();
  restarts_ = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    status_ = Status();
  }
  Start(bridges);
}

void TorService::StopWorker() {
  ++generation_;  // Anything the old session posted is now stale.
  stop_ = true;
  {
    std::lock_guard<std::mutex> guard(lock_);
    alive_ = false;
    connected_ = false;
    if (controller_) {
      controller_->Close();  // tor exits once its owning connection closes.
    }
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  stop_ = false;
}

void TorService::Shutdown() {
  CEF_REQUIRE_UI_THREAD();
  StopWorker();
  {
    std::lock_guard<std::mutex> guard(lock_);
    status_ = Status();
  }
  observers_.clear();
}

void TorService::NewIdentity(std::function<void(bool ok)> done) {
  CEF_REQUIRE_UI_THREAD();
  std::shared_ptr<tor::Controller> controller;
  {
    std::lock_guard<std::mutex> guard(lock_);
    controller = controller_;
  }
  auto finish = [done](bool ok) {
    CefPostTask(TID_UI, base::BindOnce([](std::function<void(bool)> f, bool ok) { f(ok); },
                                       done, ok));
  };
  if (!controller || !connected() ||
      !controller->Command("SIGNAL NEWNYM", [finish](const tor::Reply& r) { finish(r.ok()); })) {
    finish(false);
  }
}

void TorService::Post(int generation, Status status, int socks_port) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](std::weak_ptr<TorService> weak, int generation, Status status,
                             int socks_port) {
                            if (std::shared_ptr<TorService> self = weak.lock()) {
                              self->OnStatus(generation, std::move(status), socks_port);
                            }
                          },
                          weak_from_this(), generation, std::move(status), socks_port));
}

void TorService::OnStatus(int generation, Status status, int socks_port) {
  CEF_REQUIRE_UI_THREAD();
  if (generation != generation_.load()) {
    return;  // From a session that has since been stopped.
  }
  if (socks_port != 0 && socks_port != port_applied_) {
    if (apply_port_ && apply_port_(socks_port)) {
      port_applied_ = socks_port;
    } else {
      port_applied_ = 0;
      status = Status{State::kError, 0, "TorFox could not send Tor windows through Tor."};
    }
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    status_ = status;
    // Fail closed: only a live, authenticated session whose SOCKS port the
    // Tor context uses may count as connected.
    connected_ = status.state == State::kConnected && alive_ && port_applied_ != 0;
  }
  test_log::Event("tor_state", {{"state", StateName(status.state)},
                                {"progress", status.progress},
                                {"message", status.message},
                                {"connected", connected()}});
  if (status.state == State::kError && status.retry && restarts_ < kMaxAutomaticRestarts) {
    const int delay = kRestartDelayMs[restarts_++];
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(
            [](std::weak_ptr<TorService> weak, int generation) {
              std::shared_ptr<TorService> self = weak.lock();
              if (self && self->generation_.load() == generation &&
                  self->status().state == State::kError) {
                test_log::Event("tor_restart", {{"attempt", self->restarts_}});
                self->Start(self->bridges_);
              }
            },
            weak_from_this(), generation),
        delay);
  }
  if (status.state == State::kConnected) {
    restarts_ = 0;
  }
  // Observers may add or remove observers.
  std::vector<std::function<void()>> copy;
  for (auto& [id, observer] : observers_) {
    copy.push_back(observer);
  }
  for (auto& observer : copy) {
    observer();
  }
}

void TorService::Run(int generation, BridgeConfig bridges) {
  Session(generation, bridges);
}

bool TorService::Session(int generation, const BridgeConfig& cfg) {
  auto fail = [&](std::string message, bool retry = false) {
    Status s{State::kError, 0, std::move(message)};
    s.retry = retry;
    Post(generation, std::move(s));
    return false;
  };
  const platform::TorPaths& paths = options_.paths;

  // Bridges, checked before tor starts: if the user asked for bridges, tor
  // never starts without them.
  std::vector<tor::Bridge> bridges;
  if (cfg.mode == "builtin") {
    ReadResult config = ReadFileLimited(paths.pt_config, 1 << 20);
    if (config.status != ReadStatus::kOk) {
      return fail("TorFox's built-in bridges could not be read.");
    }
    std::string why;
    bridges = tor::BuiltinBridges(config.data, cfg.builtin_type, &why);
    if (bridges.empty()) {
      return fail("The built-in bridges could not be used: " + why);
    }
  } else if (cfg.mode == "custom") {
    tor::BridgeList list = tor::ParseBridgeLines(cfg.custom);
    if (!list.errors.empty()) {
      return fail("Your bridges could not be used. " + list.errors.front());
    }
    if (list.bridges.empty()) {
      return fail("Settings > Tor is set to use your own bridges, but none are entered.");
    }
    bridges = std::move(list.bridges);
  }

  std::error_code ec;
  std::filesystem::create_directories(options_.state_dir, ec);
  const std::filesystem::path port_file = options_.state_dir / "control-port";
  const std::filesystem::path cookie_file = options_.state_dir / "control_auth_cookie";
  std::filesystem::remove(port_file, ec);
  std::filesystem::remove(cookie_file, ec);
  const std::filesystem::path state = platform::AsciiPath(options_.state_dir);
  if (state.empty()) {
    return fail("Tor cannot use TorFox's profile folder, because its name has characters tor "
                "cannot read.");
  }
  tor::TorrcOptions o;
  o.data_dir = state;
  o.control_port_file = state / "control-port";
  if (std::filesystem::exists(paths.geoip, ec)) {
    o.geoip = platform::AsciiPath(paths.geoip);
  }
  if (std::filesystem::exists(paths.geoip6, ec)) {
    o.geoip6 = platform::AsciiPath(paths.geoip6);
  }
  o.pt_client = paths.pt_client;
  o.owning_pid = platform::CurrentProcessId();
  o.bridges = std::move(bridges);
  if (options_.notice_log) {
    o.notice_log = state / "tor-notices.log";
  }
  std::string error;
  const std::string torrc = tor::BuildTorrc(o, &error);
  if (torrc.empty()) {
    return fail("TorFox could not configure Tor: " + error + ".");
  }
  if (!WriteFileAtomically(options_.state_dir / "torrc", torrc) ||
      !WriteFileAtomically(options_.state_dir / "torrc-defaults", "")) {
    return fail("TorFox could not save Tor's configuration.");
  }

  const bool fake = !options_.test_exe.empty();
  std::vector<std::string> args = options_.test_args;
  args.push_back("-f");
  args.push_back((state / "torrc").string());
  // An empty defaults file: tor must not read one from anywhere else.
  args.push_back("--defaults-torrc");
  args.push_back((state / "torrc-defaults").string());
  std::unique_ptr<platform::ChildProcess> child =
      platform::LaunchHidden(fake ? options_.test_exe : paths.exe, args, paths.dir, &error);
  if (!child) {
    return fail("Tor could not be started: " + error + ".", true);
  }
  test_log::Event("tor_started", {{"pid", child->pid()}, {"fake", fake}});

  int control_port = 0;
  for (int waited = 0; waited < kStartTimeoutMs && control_port == 0; waited += 100) {
    if (Stopping()) {
      return true;
    }
    int code = 0;
    if (child->Exited(&code)) {
      test_log::Event("tor_stopped", {{"why", "exited while starting"}, {"code", code}});
      return fail("Tor stopped while it was starting (exit code " + std::to_string(code) + ").",
                  true);
    }
    ReadResult f = ReadFileLimited(port_file, 256);
    if (f.status == ReadStatus::kOk) {
      if (std::optional<int> port = tor::ParseControlPortFile(f.data)) {
        control_port = *port;
        break;
      }
    }
    Sleep(100);
  }
  if (control_port == 0) {
    return fail("Tor did not start in time.", true);
  }
  ReadResult cookie = ReadFileLimited(cookie_file, 64);
  if (cookie.status != ReadStatus::kOk || cookie.data.size() != 32) {
    return fail("Tor's authentication cookie could not be read.", true);
  }
  std::unique_ptr<tor::Channel> channel = tor::ConnectLoopback(control_port, 5000, &error);
  if (!channel) {
    return fail("TorFox could not reach Tor: " + error + ".", true);
  }
  auto controller = std::make_shared<tor::Controller>(std::move(channel));
  {
    std::lock_guard<std::mutex> guard(lock_);
    controller_ = controller;
  }
  if (Stopping()) {
    controller->Close();
  }
  tor::Controller::SetupResult setup = controller->Setup(cookie.data);
  if (!setup.ok) {
    {
      std::lock_guard<std::mutex> guard(lock_);
      controller_.reset();
    }
    if (Stopping()) {
      return true;
    }
    return fail("TorFox could not take control of Tor: " + setup.error + ".", true);
  }
  test_log::Event("tor_controlled", {{"version", setup.tor_version},
                                     {"socks_port", setup.socks_port},
                                     {"progress", setup.bootstrap.progress}});
  {
    std::lock_guard<std::mutex> guard(lock_);
    alive_ = !Stopping();
    version_ = setup.tor_version;
  }
  auto status_of = [](const tor::Bootstrap& b) {
    Status s;
    s.state = b.progress >= 100 ? State::kConnected : State::kConnecting;
    s.progress = b.progress;
    s.message = b.warning.empty() ? b.summary : b.warning;
    return s;
  };
  Post(generation, status_of(setup.bootstrap), setup.socks_port);
  const std::string why = controller->Run([&](const tor::Reply& event) {
    for (const std::string& line : event.lines) {
      if (std::optional<tor::Bootstrap> b = tor::ParseBootstrap(line)) {
        test_log::Event("tor_bootstrap", {{"progress", b->progress}, {"tag", b->tag}});
        Post(generation, status_of(*b));
      }
    }
  });
  {
    // First: no Tor window may send another request.
    std::lock_guard<std::mutex> guard(lock_);
    alive_ = false;
    connected_ = false;
    controller_.reset();
  }
  if (!child->WaitForExit(3000)) {
    child->Kill();
  }
  int code = 0;
  child->Exited(&code);
  test_log::Event("tor_stopped", {{"why", why}, {"code", code}, {"requested", Stopping()}});
  if (Stopping()) {
    return true;
  }
  return fail("Tor stopped unexpectedly.", true);
}

}  // namespace torfox
