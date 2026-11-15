// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/privacy/external_protocols.h"

namespace torfox::external_protocols {

namespace {

constexpr std::string_view kSchemes[] = {"mailto", "tel"};

bool SafeChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u <= 0x20 || u >= 0x7F) {
    return false;  // Controls, space, DEL and anything non-ASCII.
  }
  switch (c) {
    case '"':
    case '\'':
    case '\\':
    case '<':
    case '>':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
      return false;
    default:
      return true;
  }
}

}  // namespace

std::string_view LaunchableScheme(std::string_view url) {
  if (url.size() > kMaxUrlLength) {
    return {};
  }
  for (std::string_view scheme : kSchemes) {
    if (url.size() > scheme.size() + 1 && url.substr(0, scheme.size()) == scheme &&
        url[scheme.size()] == ':') {
      for (char c : url) {
        if (!SafeChar(c)) {
          return {};
        }
      }
      return scheme;
    }
  }
  return {};
}

}  // namespace torfox::external_protocols
