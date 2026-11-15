// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/update_service.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/cef_urlrequest.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/context_manager.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/core/update/sha256.h"
#include "src/core/util/file_io.h"

namespace torfox {

namespace {

constexpr int kMaxRedirects = 5;
constexpr int64_t kCheckInterval = 24 * 60 * 60;  // Seconds: once a day.
constexpr int kTickMs = 60 * 60 * 1000;          // Look again every hour.

int64_t Now() {
  return static_cast<int64_t>(std::time(nullptr));
}

// The host of a URL, for messages.
std::string HostOf(const std::string& url) {
  const std::size_t start = url.find("://");
  if (start == std::string::npos) {
    return "an unknown address";
  }
  const std::size_t end = url.find_first_of("/?#", start + 3);
  return url.substr(start + 3, std::min<std::size_t>(end, 200) - start - 3);
}

}  // namespace

// One download, following redirects only to allowed hosts. Kept in memory
// (up to |max_bytes|) or streamed to |sink| while it is hashed.
class UpdateService::Fetch : public CefURLRequestClient {
 public:
  struct Result {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string body;
    uint64_t size = 0;
    std::string sha256;  // Of what was received (|sink| fetches).
  };
  using Done = std::function<void(Result)>;

  Fetch(update::Endpoints endpoints, CefRefPtr<CefRequestContext> context, uint64_t max_bytes,
        std::filesystem::path sink, std::function<void(uint64_t)> progress, Done done)
      : endpoints_(std::move(endpoints)),
        context_(std::move(context)),
        max_bytes_(max_bytes),
        sink_(std::move(sink)),
        progress_(std::move(progress)),
        done_cb_(std::move(done)) {}

  void Start(const std::string& url) { Issue(url); }

  void Cancel() {
    finished_ = true;
    if (request_) {
      request_->Cancel();
    }
    request_ = nullptr;
    out_.close();
    done_cb_ = nullptr;
  }

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    // Not compared with |request_|: CEF hands each callback a new wrapper
    // object. One request runs at a time (a redirect's starts after the
    // previous one completed), and nothing runs once finished.
    if (finished_ || !request_) {
      return;
    }
    CefRefPtr<CefResponse> response = request->GetResponse();
    Result r;
    r.status = response ? response->GetStatus() : 0;
    if (r.status >= 300 && r.status < 400) {
      // UR_FLAG_STOP_ON_REDIRECT: TorFox decides whether to follow.
      const std::string location = response->GetHeaderByName("Location").ToString();
      const bool allowed = update::IsAllowedUrl(endpoints_, location);
      test_log::Event("update_redirect", {{"status", r.status},
                                          {"to", location.substr(0, 300)},
                                          {"allowed", allowed}});
      if (++redirects_ > kMaxRedirects) {
        r.error = "TorFox was redirected too many times";
      } else if (!allowed) {
        r.error = "TorFox was sent to " + HostOf(location) +
                  ", which is not one of GitHub's servers, and stopped";
      } else {
        request_ = nullptr;
        CefPostTask(TID_UI, base::BindOnce(&Fetch::Issue, CefRefPtr<Fetch>(this), location));
        return;
      }
      Finish(std::move(r));
      return;
    }
    if (write_failed_) {
      r.error = "the update could not be saved on this computer";
    } else if (overflow_) {
      r.error = "the server sent more data than expected";
    } else if (request->GetRequestStatus() != UR_SUCCESS) {
      r.error = "TorFox could not reach " + HostOf(url_) + " (network error " +
                std::to_string(static_cast<int>(request->GetRequestError())) + ")";
    } else if (r.status != 200) {
      r.error = HostOf(url_) + " answered with HTTP status " + std::to_string(r.status);
    } else {
      r.ok = true;
      r.size = received_;
      r.body = std::move(body_);
      if (!sink_.empty()) {
        r.sha256 = hash_.FinalHex();
      }
    }
    Finish(std::move(r));
  }

  void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}

  void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}

  void OnDownloadData(CefRefPtr<CefURLRequest> request, const void* data,
                      size_t length) override {
    if (finished_ || !request_ || overflow_ || write_failed_) {
      return;
    }
    received_ += length;
    if (received_ > max_bytes_) {
      overflow_ = true;
      request->Cancel();
      return;
    }
    const std::string_view chunk(static_cast<const char*>(data), length);
    if (sink_.empty()) {
      body_.append(chunk);
    } else {
      out_.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (!out_) {
        write_failed_ = true;
        request->Cancel();
        return;
      }
      hash_.Update(chunk);
    }
    if (progress_) {
      progress_(received_);
    }
  }

  bool GetAuthCredentials(bool, const CefString&, int, const CefString&, const CefString&,
                          CefRefPtr<CefAuthCallback>) override {
    return false;  // Never.
  }

 private:
  void Issue(const std::string& url) {
    if (finished_) {
      return;
    }
    url_ = url;
    if (!update::IsAllowedUrl(endpoints_, url)) {
      Result r;
      r.error = "TorFox refused to contact " + HostOf(url);
      Finish(std::move(r));
      return;
    }
    // A redirect starts over: only the final answer counts.
    received_ = 0;
    body_.clear();
    hash_ = update::Sha256();
    overflow_ = false;
    if (!sink_.empty()) {
      out_.close();
      out_.clear();
      out_.open(sink_, std::ios::binary | std::ios::trunc);
      if (!out_) {
        Result r;
        r.error = "the update could not be saved on this computer";
        Finish(std::move(r));
        return;
      }
    }
    CefRefPtr<CefRequest> request = CefRequest::Create();
    request->SetURL(url);
    request->SetMethod("GET");
    // No cookies or stored credentials (not asked for), no cache, and no
    // redirects TorFox has not checked.
    request->SetFlags(UR_FLAG_STOP_ON_REDIRECT | UR_FLAG_DISABLE_CACHE | UR_FLAG_NO_RETRY_ON_5XX);
    CefRequest::HeaderMap headers;
    headers.emplace("Accept", sink_.empty() ? "application/vnd.github+json, application/json"
                                            : "application/octet-stream");
    headers.emplace("User-Agent", "TorFox-Updater");
    request->SetHeaderMap(headers);
    test_log::Event("update_fetch", {{"url", url.substr(0, 300)}});
    request_ = CefURLRequest::Create(request, this, context_);
    if (!request_) {
      Result r;
      r.error = "TorFox could not start the request";
      Finish(std::move(r));
    }
  }

  void Finish(Result r) {
    CefRefPtr<Fetch> self(this);
    finished_ = true;
    request_ = nullptr;
    out_.close();
    if (Done done = std::move(done_cb_)) {
      done(std::move(r));
    }
  }

  const update::Endpoints endpoints_;
  CefRefPtr<CefRequestContext> context_;
  const uint64_t max_bytes_;
  const std::filesystem::path sink_;
  std::function<void(uint64_t)> progress_;
  Done done_cb_;

  CefRefPtr<CefURLRequest> request_;
  std::string url_;
  int redirects_ = 0;
  bool finished_ = false;
  bool overflow_ = false;
  bool write_failed_ = false;
  uint64_t received_ = 0;
  std::string body_;
  std::ofstream out_;
  update::Sha256 hash_;

  IMPLEMENT_REFCOUNTING(Fetch);
};

UpdateService::UpdateService(SettingsService* settings, ContextManager* contexts,
                             const std::filesystem::path& user_data_dir, Config config)
    : settings_(settings),
      contexts_(contexts),
      dir_(user_data_dir / "Updates"),
      state_file_(user_data_dir / "update.json"),
      config_(std::move(config)) {
  key_ = update::ParsePublicKey(config_.public_key);
  std::optional<update::Version> installed = update::ParseVersion(config_.installed_version);
  configured_ = key_ && installed && update::IsValidRepository(config_.repository) &&
                !config_.platform.empty();
  installed_ = installed.value_or(update::Version{});
  const ReadResult r = ReadFileLimited(state_file_, update::kMaxSavedStateBytes);
  if (r.status == ReadStatus::kOk) {
    saved_ = update::ParseSavedState(r.data);
  }
  status_.last_check = saved_.last_check;
  if (!configured_) {
    status_.state = State::kNotConfigured;
    status_.message =
        config_.public_key.empty()
            ? "This build of TorFox was made without an update signing key, so it cannot check "
              "updates and never looks for them."
            : "This build of TorFox has an invalid update configuration and never looks for "
              "updates.";
  } else if (!Enabled()) {
    status_.state = State::kOff;
  }
}

UpdateService::~UpdateService() {
  Shutdown();
}

const char* UpdateService::StateName(State state) {
  switch (state) {
    case State::kNotConfigured:
      return "not_configured";
    case State::kOff:
      return "off";
    case State::kIdle:
      return "idle";
    case State::kChecking:
      return "checking";
    case State::kUpToDate:
      return "up_to_date";
    case State::kAvailable:
      return "available";
    case State::kDownloading:
      return "downloading";
    case State::kReady:
      return "ready";
    case State::kError:
      return "error";
  }
  return "error";
}

bool UpdateService::Enabled() const {
  return settings_ && settings_->store().GetBool("updates.enabled");
}

bool UpdateService::AutoInstall() const {
  return Enabled() && settings_->store().GetBool("updates.autoInstall");
}

void UpdateService::Start() {
  CEF_REQUIRE_UI_THREAD();
  // Downloads of an earlier session (installed, abandoned or partial).
  std::error_code ec;
  std::filesystem::remove_all(dir_, ec);
  test_log::Event("update_service", {{"configured", configured_},
                                     {"state", StateName(status_.state)},
                                     {"repository", config_.repository},
                                     {"platform", config_.platform}});
  if (!configured_) {
    return;
  }
  if (settings_) {
    settings_observer_ = settings_->AddObserver([this](const std::string& key) {
      if (key.rfind("updates.", 0) == 0) {
        ApplySettings();
      }
    });
  }
  const int generation = ++generation_;
  std::weak_ptr<bool> alive = alive_;
  CefPostDelayedTask(TID_UI, base::BindOnce(
                                 [](UpdateService* self, std::weak_ptr<bool> alive, int gen) {
                                   if (alive.lock()) {
                                     self->Tick(gen);
                                   }
                                 },
                                 this, alive, generation),
                     config_.first_check_delay_ms);
}

void UpdateService::ApplySettings() {
  if (shut_down_ || !configured_) {
    return;
  }
  if (!Enabled()) {
    CancelFetch();
    asset_ = {};
    asset_url_.clear();
    SetState(State::kOff);
    return;
  }
  if (status_.state == State::kOff) {
    SetState(State::kIdle);
  }
  // Automatic installs turned on with an update waiting: fetch it now.
  if (AutoInstall() && status_.state == State::kAvailable) {
    Download();
  }
  // Automatic checks may have been turned on: look again soon.
  const int generation = ++generation_;
  std::weak_ptr<bool> alive = alive_;
  CefPostDelayedTask(TID_UI, base::BindOnce(
                                 [](UpdateService* self, std::weak_ptr<bool> alive, int gen) {
                                   if (alive.lock()) {
                                     self->Tick(gen);
                                   }
                                 },
                                 this, alive, generation),
                     std::min(config_.first_check_delay_ms, 5000));
}

void UpdateService::Tick(int generation) {
  if (shut_down_ || generation != generation_) {
    return;
  }
  const int64_t last = std::max(saved_.last_check, last_attempt_);
  if (Enabled() && settings_->store().GetBool("updates.autoCheck") &&
      Now() - last >= kCheckInterval) {
    StartCheck(true);
  }
  std::weak_ptr<bool> alive = alive_;
  CefPostDelayedTask(TID_UI, base::BindOnce(
                                 [](UpdateService* self, std::weak_ptr<bool> alive, int gen) {
                                   if (alive.lock()) {
                                     self->Tick(gen);
                                   }
                                 },
                                 this, alive, generation),
                     kTickMs);
}

void UpdateService::Check() {
  CEF_REQUIRE_UI_THREAD();
  StartCheck(false);
}

void UpdateService::StartCheck(bool automatic) {
  if (shut_down_ || !configured_ || !Enabled()) {
    return;
  }
  switch (status_.state) {
    case State::kChecking:
    case State::kDownloading:
    case State::kReady:
      return;  // Busy, or already holding a checked update.
    default:
      break;
  }
  last_attempt_ = Now();
  asset_ = {};
  asset_url_.clear();
  status_.version.clear();
  status_.notes.clear();
  status_.size = status_.received = 0;
  SetState(State::kChecking);
  WithContext([this, automatic] {
    Get(update::LatestReleaseUrl(config_.endpoints, config_.repository),
        update::kMaxReleaseJsonBytes,
        [this, automatic](bool ok, int status, const std::string& error, const std::string& body) {
          if (!ok) {
            Fail(status == 404 ? "No TorFox release has been published yet."
                               : "TorFox could not look for updates: " + error + ".",
                 "release", automatic);
            return;
          }
          std::string why;
          std::optional<update::LatestRelease> latest =
              update::ParseLatestRelease(config_.endpoints, config_.repository, body, &why);
          if (!latest) {
            Fail("TorFox could not read the latest release: " + why + ".", "release", automatic);
            return;
          }
          OnRelease(automatic, *latest);
        });
  });
}

void UpdateService::OnRelease(bool automatic, const update::LatestRelease& latest) {
  Get(latest.signature_url, update::kMaxSignatureFileBytes,
      [this, automatic, latest](bool ok, int, const std::string& error,
                                const std::string& signature) {
        if (!ok) {
          Fail("TorFox could not download the release's signature: " + error + ".", "signature",
               automatic);
          return;
        }
        Get(latest.manifest_url, update::kMaxManifestBytes,
            [this, automatic, latest, signature](bool ok2, int, const std::string& error2,
                                                 const std::string& manifest) {
              if (!ok2) {
                Fail("TorFox could not download the release manifest: " + error2 + ".",
                     "manifest", automatic);
                return;
              }
              OnManifest(automatic, latest, signature, manifest);
            });
      });
}

void UpdateService::OnManifest(bool automatic, const update::LatestRelease& latest,
                               const std::string& signature, const std::string& manifest) {
  const update::ManifestCheck c = update::CheckManifest(*key_, manifest, signature);
  if (!c.ok) {
    Fail("The update could not be verified, so TorFox ignored it (" + c.error + ").", "verify",
         automatic);
    return;
  }
  const update::Offer offer =
      update::Decide(c.release, installed_, saved_.newest_seen, config_.platform);
  // Remembered only once verified: a forged manifest can never move it.
  saved_.newest_seen = std::max(saved_.newest_seen, c.release.timestamp);
  saved_.last_check = Now();
  status_.last_check = saved_.last_check;
  // The record of the newest release seen is what stops an old release from
  // being replayed after a restart: no update is offered unless it is saved.
  if (!SaveState()) {
    Fail("TorFox could not save its record of the releases it has seen (update.json in its data "
         "folder), so it offers no updates until it can.",
         "state", automatic);
    return;
  }
  const std::string version = update::VersionString(c.release.version);
  if (offer.replayed) {
    Fail("GitHub offered TorFox " + version +
             ", a release older than one TorFox has already seen, so TorFox ignored it.",
         "replay", automatic);
    return;
  }
  test_log::Event("update_check", {{"result", offer.update ? "available" : "up_to_date"},
                                   {"version", version},
                                   {"reason", offer.reason},
                                   {"automatic", automatic}});
  if (!offer.update) {
    SetState(State::kUpToDate, offer.reason);
    return;
  }
  auto url = latest.asset_urls.find(offer.asset.name);
  if (url == latest.asset_urls.end()) {
    Fail("TorFox " + version + " was released without its file for this computer.", "asset",
         automatic);
    return;
  }
  asset_ = offer.asset;
  asset_url_ = url->second;
  status_.version = version;
  status_.notes = c.release.notes;
  status_.size = asset_.size;
  SetState(State::kAvailable);
  if (AutoInstall()) {
    // Like any download it is checked against the signed size and SHA-256;
    // the toolbar says when it is ready.
    Download();
    return;
  }
  if (automatic && notify && settings_->store().GetBool("updates.notify")) {
    notify(version);
  }
}

void UpdateService::Download() {
  CEF_REQUIRE_UI_THREAD();
  if (shut_down_ || !configured_ || !Enabled() || asset_.name.empty() ||
      (status_.state != State::kAvailable && status_.state != State::kError)) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  std::filesystem::remove(PartPath(), ec);
  std::filesystem::remove(FinalPath(), ec);
  status_.received = 0;
  SetState(State::kDownloading);
  WithContext([this] {
    std::weak_ptr<bool> alive = alive_;
    fetch_ = new Fetch(
        config_.endpoints, context_, asset_.size, PartPath(),
        [this, alive](uint64_t received) {
          if (!alive.lock()) {
            return;
          }
          // Progress in whole percent, not for every packet.
          const uint64_t before = status_.size ? status_.received * 100 / status_.size : 0;
          status_.received = received;
          if (status_.size && received * 100 / status_.size != before) {
            Notify();
          }
        },
        [this, alive](Fetch::Result r) {
          if (!alive.lock()) {
            return;
          }
          fetch_ = nullptr;
          std::error_code ec2;
          std::string problem;
          if (!r.ok) {
            problem = "The download failed: " + r.error + ".";
          } else if (r.size != asset_.size) {
            problem = "The download was incomplete.";
          } else if (r.sha256 != asset_.sha256) {
            problem = "The downloaded file does not match the signed release, so TorFox deleted it.";
          } else if (!RenameReplacing(PartPath(), FinalPath())) {
            problem = "The update could not be saved on this computer.";
          }
          if (!problem.empty()) {
            std::filesystem::remove(PartPath(), ec2);
            std::filesystem::remove(FinalPath(), ec2);
            test_log::Event("update_download", {{"result", "failed"},
                                                {"error", problem},
                                                {"size", r.size},
                                                {"sha256", r.sha256}});
            SetState(State::kError, problem);
            return;
          }
          test_log::Event("update_download",
                          {{"result", "ok"}, {"size", r.size}, {"sha256", r.sha256},
                           {"file", FinalPath().filename().string()}});
          SetState(State::kReady);
        });
    fetch_->Start(asset_url_);
  });
}

bool UpdateService::PrepareInstall(bool relaunch, std::string* error) {
  CEF_REQUIRE_UI_THREAD();
  if (status_.state != State::kReady) {
    *error = "No update is ready to install.";
    return false;
  }
  const std::optional<update::FileDigest> d = update::HashFile(FinalPath(), asset_.size);
  if (!d || d->size != asset_.size || d->sha256 != asset_.sha256) {
    std::error_code ec;
    std::filesystem::remove(FinalPath(), ec);
    *error = "The downloaded update changed on this computer, so TorFox deleted it.";
    test_log::Event("update_install", {{"ok", false}, {"error", *error}, {"relaunch", relaunch}});
    SetState(State::kError, *error);
    return false;
  }
  handoff_ = Handoff{FinalPath(), asset_.sha256, asset_.size, relaunch, status_.version};
  test_log::Event("update_install", {{"ok", true},
                                     {"file", FinalPath().filename().string()},
                                     {"sha256", asset_.sha256},
                                     {"size", asset_.size},
                                     {"relaunch", relaunch}});
  return true;
}

bool UpdateService::PrepareInstallOnExit() {
  CEF_REQUIRE_UI_THREAD();
  if (shut_down_ || handoff_ || status_.state != State::kReady || !AutoInstall()) {
    return false;
  }
  std::string error;
  return PrepareInstall(/*relaunch=*/false, &error);
}

std::optional<UpdateService::Handoff> UpdateService::TakeHandoff() {
  std::optional<Handoff> h = std::move(handoff_);
  handoff_.reset();
  return h;
}

void UpdateService::Shutdown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  ++generation_;
  CancelFetch();
  if (settings_ && settings_observer_) {
    settings_->RemoveObserver(settings_observer_);
  }
  settings_observer_ = 0;
  context_ = nullptr;
  context_waiting_.clear();
  observers_.clear();
  alive_.reset();
}

UpdateService::Status UpdateService::status() const {
  return status_;
}

int UpdateService::AddObserver(std::function<void()> observer) {
  const int id = next_observer_++;
  observers_[id] = std::move(observer);
  return id;
}

void UpdateService::RemoveObserver(int id) {
  observers_.erase(id);
}

void UpdateService::WithContext(std::function<void()> then) {
  if (context_) {
    then();
    return;
  }
  context_waiting_.push_back(std::move(then));
  if (context_pending_ || !contexts_) {
    return;
  }
  context_pending_ = true;
  std::weak_ptr<bool> alive = alive_;
  // Its own off-the-record context: no cookies or cache, nothing shared
  // with any window, the same network settings as normal windows.
  contexts_->CreatePrivate(
      [this, alive](CefRefPtr<CefRequestContext> context, const std::string& error) {
        if (!alive.lock()) {
          return;
        }
        context_pending_ = false;
        std::vector<std::function<void()>> waiting = std::move(context_waiting_);
        context_waiting_.clear();
        if (!context) {
          Fail("TorFox could not prepare its update connection: " + error, "context", false);
          return;
        }
        context_ = context;
        for (std::function<void()>& w : waiting) {
          w();
        }
      });
}

void UpdateService::Get(const std::string& url, uint64_t max_bytes, Body done) {
  std::weak_ptr<bool> alive = alive_;
  fetch_ = new Fetch(config_.endpoints, context_, max_bytes, {}, nullptr,
                     [this, alive, done = std::move(done)](Fetch::Result r) {
                       if (!alive.lock()) {
                         return;
                       }
                       fetch_ = nullptr;
                       done(r.ok, r.status, r.error, r.body);
                     });
  fetch_->Start(url);
}

void UpdateService::Fail(const std::string& message, const char* stage, bool automatic) {
  test_log::Event("update_check", {{"result", "failed"},
                                   {"stage", stage},
                                   {"error", message},
                                   {"automatic", automatic}});
  asset_ = {};
  asset_url_.clear();
  SetState(State::kError, message);
}

void UpdateService::SetState(State state, std::string message) {
  status_.state = state;
  status_.message = std::move(message);
  Notify();
}

bool UpdateService::SaveState() {
  if (!WriteFileAtomically(state_file_, update::SerializeSavedState(saved_))) {
    test_log::Event("update_state_save_failed");
    return false;
  }
  return true;
}

void UpdateService::Notify() {
  // Copy: an observer may remove itself.
  std::map<int, std::function<void()>> observers = observers_;
  for (auto& [id, observer] : observers) {
    observer();
  }
}

void UpdateService::CancelFetch() {
  if (fetch_) {
    fetch_->Cancel();
    fetch_ = nullptr;
  }
}

std::filesystem::path UpdateService::PartPath() const {
  return dir_ / (asset_.name + ".part");
}

std::filesystem::path UpdateService::FinalPath() const {
  return dir_ / asset_.name;
}

}  // namespace torfox
