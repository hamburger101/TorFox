// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/json.h"

namespace torfox {

namespace {

// Returns the maximum nesting depth of arrays/objects in |text|, ignoring
// brackets inside strings. Malformed input is left for the real parser.
int NestingDepth(std::string_view text) {
  int depth = 0;
  int max_depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
      if (depth > max_depth) {
        max_depth = depth;
      }
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return max_depth;
}

}  // namespace

std::optional<Json> ParseJson(std::string_view text, std::size_t max_bytes, int max_depth) {
  if (text.size() > max_bytes) {
    return std::nullopt;
  }
  if (NestingDepth(text) > max_depth) {
    return std::nullopt;
  }
  Json value = Json::parse(text.begin(), text.end(), /*cb=*/nullptr,
                           /*allow_exceptions=*/false, /*ignore_comments=*/false);
  if (value.is_discarded()) {
    return std::nullopt;
  }
  return value;
}

std::string DumpJson(const Json& value, bool pretty) {
  return value.dump(pretty ? 2 : -1, ' ', /*ensure_ascii=*/false,
                    Json::error_handler_t::replace);
}

const Json* GetMember(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  auto it = object.find(key);
  if (it == object.end()) {
    return nullptr;
  }
  return &*it;
}

std::optional<std::string> GetString(const Json& object, std::string_view key) {
  const Json* v = GetMember(object, key);
  if (!v || !v->is_string()) {
    return std::nullopt;
  }
  return v->get_ref<const std::string&>();
}

std::optional<bool> GetBool(const Json& object, std::string_view key) {
  const Json* v = GetMember(object, key);
  if (!v || !v->is_boolean()) {
    return std::nullopt;
  }
  return v->get<bool>();
}

std::optional<long long> GetInt(const Json& object, std::string_view key) {
  const Json* v = GetMember(object, key);
  if (!v || !v->is_number_integer()) {
    return std::nullopt;
  }
  return v->get<long long>();
}

}  // namespace torfox
