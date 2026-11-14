// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/privacy_service.h"

#include <utility>

#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/settings_service.h"
#include "src/core/settings/schema.h"

namespace torfox {

namespace {

constexpr char kHttpsFirst[] = "privacy.httpsFirst";
constexpr char kBlockTrackers[] = "privacy.blockTrackers";
constexpr char kBlockCookies[] = "privacy.blockThirdPartyCookies";
constexpr char kBlockPopups[] = "privacy.blockPopups";

}  // namespace

PrivacyService::PrivacyService(SettingsService* settings,
                               std::optional<https_first::TestPorts> test_ports)
    : settings_(settings), test_ports_(std::move(test_ports)) {
  Refresh(std::string());
  if (settings_) {
    observer_id_ = settings_->AddObserver([this](const std::string& key) { Refresh(key); });
  }
}

PrivacyService::~PrivacyService() {
  if (settings_ && observer_id_) {
    settings_->RemoveObserver(observer_id_);
  }
}

void PrivacyService::Refresh(const std::string& key) {
  // Without settings every protection stays on (the safer default).
  auto get = [this](const char* k) { return !settings_ || settings_->store().GetBool(k); };
  https_first_ = get(kHttpsFirst);
  block_trackers_ = get(kBlockTrackers);
  const bool cookies = get(kBlockCookies);
  const bool changed = cookies != block_third_party_cookies_.exchange(cookies);
  block_popups_ = get(kBlockPopups);
  if ((changed || key == kBlockCookies) && cookie_listener_) {
    cookie_listener_(cookies);
  }
}

PrivacyService::Permission PrivacyService::PermissionFor(const std::string& setting) const {
  if (!settings_ || setting.rfind("permissions.", 0) != 0) {
    return Permission::kDeny;
  }
  const settings::Def* def = settings::Find(setting);
  if (!def || def->status != settings::Status::kActive) {
    return Permission::kDeny;  // Location and notifications: not available.
  }
  return settings_->store().GetString(setting) == "ask" ? Permission::kAsk : Permission::kDeny;
}

}  // namespace torfox
