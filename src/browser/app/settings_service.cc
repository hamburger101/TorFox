// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/settings_service.h"

#include <utility>
#include <vector>

#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"

namespace torfox {

SettingsService::SettingsService(std::filesystem::path file) : store_(std::move(file)) {}

settings::LoadResult SettingsService::Load() {
  settings::LoadResult r = store_.Load();
  if (r.outcome == settings::LoadOutcome::kRepaired ||
      r.outcome == settings::LoadOutcome::kCorruptReset) {
    // Persist the repaired state so the bad values are gone for good.
    store_.Save();
  }
  Json notes = Json::array();
  for (const std::string& n : r.notes) {
    notes.push_back(n);
  }
  test_log::Event("settings_loaded", {{"outcome", static_cast<int>(r.outcome)}, {"notes", notes}});
  return r;
}

SettingsService::ChangeResult SettingsService::Change(const std::string& key, const Json& value) {
  const settings::Def* def = settings::Find(key);
  if (!def) {
    return ChangeResult::kUnknownKey;
  }
  if (def->status != settings::Status::kActive) {
    return ChangeResult::kNotAvailable;
  }
  const Json previous = store_.Get(key);
  switch (store_.Set(key, value)) {
    case settings::SetResult::kOk:
      break;
    case settings::SetResult::kUnchanged:
      return ChangeResult::kUnchanged;
    case settings::SetResult::kUnknownKey:
      return ChangeResult::kUnknownKey;
    case settings::SetResult::kInvalidValue:
      return ChangeResult::kInvalidValue;
  }
  const bool saved = store_.Save();
  test_log::Event("settings_saved", {{"key", key}, {"ok", saved}});
  if (!saved) {
    // Keep memory and disk in agreement: a change that cannot be saved is
    // not applied at all.
    store_.Set(key, previous);
    return ChangeResult::kSaveFailed;
  }
  // Copy: observers may remove themselves while being notified.
  std::vector<Observer> observers;
  for (auto& [id, obs] : observers_) {
    observers.push_back(obs);
  }
  for (auto& obs : observers) {
    obs(key);
  }
  return ChangeResult::kOk;
}

int SettingsService::AddObserver(Observer observer) {
  int id = next_observer_id_++;
  observers_[id] = std::move(observer);
  return id;
}

void SettingsService::RemoveObserver(int id) {
  observers_.erase(id);
}

bool SettingsService::EffectiveDark() const {
  std::string theme = store_.GetString("appearance.theme");
  if (theme == "light") {
    return false;
  }
  if (theme == "system") {
    return platform::SystemPrefersDark();
  }
  return true;
}

Json SettingsService::DescribeForUi() const {
  Json sections = Json::array();
  for (std::string_view s : settings::AllSections()) {
    sections.push_back(std::string(s));
  }
  Json list = Json::array();
  for (const settings::Def& d : settings::AllSettings()) {
    Json item = Json::object();
    item["key"] = std::string(d.key);
    item["section"] = std::string(d.section);
    item["type"] = std::string(settings::TypeName(d.type));
    item["status"] = std::string(settings::StatusName(d.status));
    item["value"] = store_.Get(d.key);
    item["default"] = settings::DefaultValue(d);
    if (d.type == settings::Type::kEnum) {
      Json values = Json::array();
      for (std::string_view v : d.enum_values) {
        values.push_back(std::string(v));
      }
      item["values"] = std::move(values);
    }
    if (d.type == settings::Type::kString) {
      item["maxLength"] = d.max_length;
    }
    list.push_back(std::move(item));
  }
  Json out = Json::object();
  out["sections"] = std::move(sections);
  out["settings"] = std::move(list);
  return out;
}

std::string_view ChangeResultName(SettingsService::ChangeResult r) {
  switch (r) {
    case SettingsService::ChangeResult::kOk:
      return "ok";
    case SettingsService::ChangeResult::kUnchanged:
      return "unchanged";
    case SettingsService::ChangeResult::kUnknownKey:
      return "unknown_key";
    case SettingsService::ChangeResult::kNotAvailable:
      return "not_available";
    case SettingsService::ChangeResult::kInvalidValue:
      return "invalid_value";
    case SettingsService::ChangeResult::kSaveFailed:
      return "save_failed";
  }
  return "unknown";
}

}  // namespace torfox
