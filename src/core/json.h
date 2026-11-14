// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_JSON_H_
#define TORFOX_CORE_JSON_H_

// TorFox is built without C++ exceptions (matching CEF). nlohmann/json is used
// in JSON_NOEXCEPTION mode, where any error that would throw aborts instead.
// Code must therefore only use the non-throwing entry points below and must
// check types before reading values.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace torfox {

using Json = nlohmann::json;

// Parses |text| strictly (no comments, valid UTF-8, single value). Returns
// nullopt if the text is larger than |max_bytes|, nests deeper than
// |max_depth|, or is not valid JSON.
std::optional<Json> ParseJson(std::string_view text, std::size_t max_bytes, int max_depth);

// Serializes without ever aborting: invalid UTF-8 is replaced with U+FFFD.
std::string DumpJson(const Json& value, bool pretty = false);

// Typed lookups that never abort. Each returns nullopt if |object| is not an
// object, |key| is missing, or the value has a different type.
std::optional<std::string> GetString(const Json& object, std::string_view key);
std::optional<bool> GetBool(const Json& object, std::string_view key);
std::optional<long long> GetInt(const Json& object, std::string_view key);
const Json* GetMember(const Json& object, std::string_view key);

}  // namespace torfox

#endif  // TORFOX_CORE_JSON_H_
