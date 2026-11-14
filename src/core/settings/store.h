// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_SETTINGS_STORE_H_
#define TORFOX_CORE_SETTINGS_STORE_H_

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/json.h"
#include "src/core/settings/schema.h"

namespace torfox::settings {

inline constexpr int kFormatVersion = 1;
inline constexpr std::size_t kMaxFileBytes = 1 << 20;

enum class LoadOutcome {
  kMissing,       // No file: defaults are in use.
  kLoaded,        // File read and every stored value was valid.
  kRepaired,      // File read; some values were invalid/unknown and were dropped.
  kCorruptReset,  // File unreadable or not valid JSON: backed up, defaults in use.
  kReadError,     // I/O error: defaults in use, file left untouched.
};

struct LoadResult {
  LoadOutcome outcome = LoadOutcome::kMissing;
  std::vector<std::string> notes;     // Human-readable list of repairs.
  std::filesystem::path backup_path;  // Set for kCorruptReset if a backup was made.
};

enum class SetResult { kOk, kUnchanged, kUnknownKey, kInvalidValue };

// Holds TorFox settings in memory and persists them as versioned JSON:
//   {"format": 1, "settings": {"appearance.theme": "dark", ...}}
// Only values that differ from the defaults are written. Not thread-safe; the
// owner serialises access.
class Store {
 public:
  explicit Store(std::filesystem::path file);

  LoadResult Load();
  bool Save() const;

  // Returns the current value (stored or default). |key| must be known.
  Json Get(std::string_view key) const;
  bool GetBool(std::string_view key) const;
  std::string GetString(std::string_view key) const;

  SetResult Set(std::string_view key, const Json& value);
  void ResetToDefaults();

  // The serialised file contents.
  std::string Serialize() const;

  const std::filesystem::path& file() const { return file_; }

 private:
  std::filesystem::path file_;
  std::map<std::string, Json, std::less<>> values_;  // Non-default values only.
};

}  // namespace torfox::settings

#endif  // TORFOX_CORE_SETTINGS_STORE_H_
