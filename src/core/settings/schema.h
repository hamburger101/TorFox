// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_SETTINGS_SCHEMA_H_
#define TORFOX_CORE_SETTINGS_SCHEMA_H_

#include <cstddef>
#include <span>
#include <string_view>

#include "src/core/json.h"

namespace torfox::settings {

enum class Type { kBool, kEnum, kString };

// kActive settings change TorFox's behaviour today. kPlanned settings are part
// of the schema (so stored values survive upgrades) but the feature behind
// them is not implemented yet; the UI shows them disabled and the bridge
// refuses to change them.
enum class Status { kActive, kPlanned };

struct Def {
  std::string_view key;
  std::string_view section;
  Type type;
  Status status;
  bool default_bool = false;
  std::string_view default_string;
  std::span<const std::string_view> enum_values;  // kEnum only
  std::size_t max_length = 0;                     // kString only
  // kString only: a further check of the text (for example, bridge lines).
  bool (*check)(std::string_view text) = nullptr;
};

// Every setting TorFox knows about, in display order. Defaults are always the
// safer choice; invalid stored values fall back to them.
std::span<const Def> AllSettings();

// Section identifiers in display order.
std::span<const std::string_view> AllSections();

const Def* Find(std::string_view key);

// True if |value| has the right JSON type and satisfies the constraints.
bool IsValid(const Def& def, const Json& value);

Json DefaultValue(const Def& def);

std::string_view TypeName(Type type);
std::string_view StatusName(Status status);

}  // namespace torfox::settings

#endif  // TORFOX_CORE_SETTINGS_SCHEMA_H_
