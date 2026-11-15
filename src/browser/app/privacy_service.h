// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_PRIVACY_SERVICE_H_
#define TORFOX_BROWSER_APP_PRIVACY_SERVICE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "src/core/adblock/sites.h"
#include "src/core/privacy/https_first.h"

namespace torfox {

class SettingsService;

// Settings > Privacy & Security, as the request handlers need it. The
// network-thread handlers (tracker blocking) cannot read the settings store,
// which belongs to the UI thread, so the switches are mirrored here as
// atomics, updated by a settings observer. UI thread unless noted.
class PrivacyService {
 public:
  // What a page asking for a capability gets.
  enum class Permission { kDeny, kAsk };

  // |test_ports| (test builds): see https_first::TestPorts.
  PrivacyService(SettingsService* settings, std::optional<https_first::TestPorts> test_ports);
  ~PrivacyService();

  // Any thread.
  bool https_first() const { return https_first_; }
  bool block_trackers() const { return block_trackers_; }
  bool block_third_party_cookies() const { return block_third_party_cookies_; }
  bool block_popups() const { return block_popups_; }
  bool block_ads() const { return block_ads_; }
  const https_first::TestPorts* test_ports() const { return test_ports_ ? &*test_ports_ : nullptr; }

  // Hosts the user chose to load over http in normal windows (each private
  // window keeps its own). Any thread.
  https_first::Exceptions& normal_exceptions() { return normal_exceptions_; }

  // Sites the user allowed ads on in normal windows (the setting
  // "privacy.adsAllowedSites"; private and Tor windows keep their own).
  // Any thread.
  const std::shared_ptr<adblock::SiteList>& normal_ads_allowed() const { return normal_ads_allowed_; }
  // Allows or blocks ads again on |host| in normal windows, and saves the
  // list. False if it could not be saved (nothing changed then).
  bool SetAdsAllowed(const std::string& host, bool allowed);

  // "permissions.camera", "permissions.microphone", "permissions.clipboard".
  // Anything unknown, or a setting that is not available, is kDeny.
  Permission PermissionFor(const std::string& setting) const;

  // Called when "Block third-party cookies" changes, to apply it to every
  // live browsing context.
  void SetCookiePolicyListener(std::function<void(bool block)> listener) {
    cookie_listener_ = std::move(listener);
  }

 private:
  void Refresh(const std::string& key);

  SettingsService* settings_;
  int observer_id_ = 0;
  std::optional<https_first::TestPorts> test_ports_;
  std::atomic<bool> https_first_{true};
  std::atomic<bool> block_trackers_{true};
  std::atomic<bool> block_third_party_cookies_{true};
  std::atomic<bool> block_popups_{true};
  std::atomic<bool> block_ads_{true};
  https_first::Exceptions normal_exceptions_;
  const std::shared_ptr<adblock::SiteList> normal_ads_allowed_ = std::make_shared<adblock::SiteList>();
  std::function<void(bool)> cookie_listener_;
};

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_PRIVACY_SERVICE_H_
