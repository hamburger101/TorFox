// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_SETTINGS_SERVICE_H_
#define TORFOX_BROWSER_APP_SETTINGS_SERVICE_H_

#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "src/core/settings/store.h"

namespace torfox {

// Owns the settings store on the UI thread. Every accepted change is written
// to disk immediately (atomic replace) so settings survive crashes and
// restarts, then observers are notified.
class SettingsService {
 public:
  using Observer = std::function<void(const std::string& key)>;

  explicit SettingsService(std::filesystem::path file);

  settings::LoadResult Load();
  const settings::Store& store() const { return store_; }

  enum class ChangeResult { kOk, kUnchanged, kUnknownKey, kNotAvailable, kInvalidValue, kSaveFailed };
  // Changes an *active* setting. Planned settings are refused with
  // kNotAvailable because nothing would honour them yet.
  ChangeResult Change(const std::string& key, const Json& value);

  int AddObserver(Observer observer);
  void RemoveObserver(int id);

  // Effective theme: "dark" or "light" ("system" resolved via the OS).
  bool EffectiveDark() const;

  // {"sections":[...],"settings":[{key,section,type,status,values?,maxLength?,value}...]}
  Json DescribeForUi() const;

 private:
  settings::Store store_;
  std::map<int, Observer> observers_;
  int next_observer_id_ = 1;
};

std::string_view ChangeResultName(SettingsService::ChangeResult r);

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_SETTINGS_SERVICE_H_
