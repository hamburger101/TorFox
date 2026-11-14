// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/settings/store.h"

#include <system_error>
#include <utility>

#include "src/core/util/file_io.h"

namespace torfox::settings {

namespace fs = std::filesystem;

namespace {

constexpr int kMaxDepth = 8;

fs::path CorruptBackupPath(const fs::path& file) {
  fs::path backup = file;
  backup += ".corrupt";
  return backup;
}

}  // namespace

Store::Store(fs::path file) : file_(std::move(file)) {}

LoadResult Store::Load(LoadMode mode) {
  values_.clear();
  LoadResult result;

  ReadResult read = ReadFileLimited(file_, kMaxFileBytes);
  if (read.status == ReadStatus::kMissing) {
    result.outcome = LoadOutcome::kMissing;
    return result;
  }
  if (read.status == ReadStatus::kError) {
    result.outcome = LoadOutcome::kReadError;
    result.notes.push_back("settings file could not be read; using defaults");
    return result;
  }

  std::optional<Json> parsed;
  if (read.status == ReadStatus::kOk) {
    parsed = ParseJson(read.data, kMaxFileBytes, kMaxDepth);
  }
  const Json* format = parsed ? GetMember(*parsed, "format") : nullptr;
  const Json* stored = parsed ? GetMember(*parsed, "settings") : nullptr;
  if (!parsed || !parsed->is_object() || !format || !format->is_number_integer() || !stored ||
      !stored->is_object()) {
    result.outcome = LoadOutcome::kCorruptReset;
    result.notes.push_back(read.status == ReadStatus::kTooLarge
                               ? "settings file is too large; reset to defaults"
                               : "settings file is not valid; reset to defaults");
    if (mode == LoadMode::kRepair) {
      fs::path backup = CorruptBackupPath(file_);
      if (RenameReplacing(file_, backup)) {
        result.backup_path = backup;
      }
    }
    return result;
  }

  result.outcome = LoadOutcome::kLoaded;
  if (format->get<long long>() != kFormatVersion) {
    result.outcome = LoadOutcome::kRepaired;
    result.notes.push_back("settings file format " + DumpJson(*format) +
                           " is not the current format; reading known settings only");
  }
  for (auto it = stored->begin(); it != stored->end(); ++it) {
    const Def* def = Find(it.key());
    if (!def) {
      result.outcome = LoadOutcome::kRepaired;
      result.notes.push_back("dropped unknown setting \"" + it.key() + "\"");
      continue;
    }
    if (!IsValid(*def, it.value())) {
      result.outcome = LoadOutcome::kRepaired;
      result.notes.push_back("invalid value for \"" + it.key() + "\"; using the default");
      continue;
    }
    if (it.value() != DefaultValue(*def)) {
      values_[it.key()] = it.value();
    }
  }
  return result;
}

std::string Store::Serialize() const {
  Json settings = Json::object();
  for (const auto& [key, value] : values_) {
    settings[key] = value;
  }
  Json root = Json::object();
  root["format"] = kFormatVersion;
  root["settings"] = std::move(settings);
  return DumpJson(root, /*pretty=*/true) + "\n";
}

bool Store::Save() const {
  return WriteFileAtomically(file_, Serialize());
}

Json Store::Get(std::string_view key) const {
  auto it = values_.find(key);
  if (it != values_.end()) {
    return it->second;
  }
  const Def* def = Find(key);
  return def ? DefaultValue(*def) : Json();
}

bool Store::GetBool(std::string_view key) const {
  Json v = Get(key);
  return v.is_boolean() ? v.get<bool>() : false;
}

std::string Store::GetString(std::string_view key) const {
  Json v = Get(key);
  return v.is_string() ? v.get<std::string>() : std::string();
}

SetResult Store::Set(std::string_view key, const Json& value) {
  const Def* def = Find(key);
  if (!def) {
    return SetResult::kUnknownKey;
  }
  if (!IsValid(*def, value)) {
    return SetResult::kInvalidValue;
  }
  if (Get(key) == value) {
    return SetResult::kUnchanged;
  }
  if (value == DefaultValue(*def)) {
    auto it = values_.find(key);
    if (it != values_.end()) {
      values_.erase(it);
    }
  } else {
    values_[std::string(key)] = value;
  }
  return SetResult::kOk;
}

void Store::ResetToDefaults() {
  values_.clear();
}

}  // namespace torfox::settings
