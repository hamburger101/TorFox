// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/session_service.h"

#include <system_error>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/settings_service.h"
#include "src/browser/app/test_log.h"
#include "src/core/util/file_io.h"

namespace torfox {

namespace {

constexpr int kSaveDelayMs = 1000;
constexpr const char* kClearHistorySetting = "privacy.clearOnExit.history";

}  // namespace

SessionService::SessionService(SettingsService* settings,
                               std::filesystem::path file,
                               Collect collect)
    : settings_(settings), file_(std::move(file)), collect_(std::move(collect)) {
  if (settings_) {
    observer_ = settings_->AddObserver([this](const std::string& key) {
      if (key != browsing::kStartupSetting && key != kClearHistorySetting) {
        return;
      }
      if (enabled()) {
        Changed();
      } else {
        Remove("setting");
      }
    });
  }
}

SessionService::~SessionService() {
  if (settings_ && observer_) {
    settings_->RemoveObserver(observer_);
  }
}

bool SessionService::enabled() const {
  return settings_ && settings_->store().GetString(browsing::kStartupSetting) == "restore" &&
         !settings_->store().GetBool(kClearHistorySetting);
}

browsing::Session SessionService::TakeStartupSession() {
  CEF_REQUIRE_UI_THREAD();
  if (!enabled()) {
    Remove("startup");
    return {};
  }
  const ReadResult r = ReadFileLimited(file_, browsing::kMaxSessionBytes);
  if (r.status != ReadStatus::kOk) {
    return {};
  }
  return browsing::ParseSession(r.data);
}

void SessionService::Changed() {
  CEF_REQUIRE_UI_THREAD();
  if (pending_ || shut_down_ || !enabled()) {
    return;
  }
  pending_ = true;
  std::weak_ptr<int> alive = alive_;
  CefPostDelayedTask(TID_UI,
                     base::BindOnce(
                         [](std::weak_ptr<int> alive, SessionService* self) {
                           if (alive.lock()) {
                             self->Save();
                           }
                         },
                         alive, this),
                     kSaveDelayMs);
}

void SessionService::Save() {
  pending_ = false;
  if (shut_down_ || !enabled()) {
    return;
  }
  const browsing::Session session = collect_ ? collect_() : browsing::Session();
  std::size_t tabs = 0;
  for (const browsing::SessionWindow& w : session) {
    tabs += w.urls.size();
  }
  if (session.empty() || tabs == 0) {
    return;  // Keep the windows that were open when the last one closed.
  }
  const bool ok = WriteFileAtomically(file_, browsing::SerializeSession(session));
  test_log::Event("session_saved", {{"windows", session.size()}, {"tabs", tabs}, {"ok", ok}});
}

void SessionService::Remove(const char* why) {
  std::error_code ec;
  const bool existed = std::filesystem::remove(file_, ec);
  if (existed || ec) {
    test_log::Event("session_removed", {{"why", why}, {"ok", !ec}});
  }
}

void SessionService::Shutdown() {
  shut_down_ = true;
  if (!enabled()) {
    Remove("exit");
  }
}

}  // namespace torfox
