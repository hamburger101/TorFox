// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UTIL_UTF8_H_
#define TORFOX_CORE_UTIL_UTF8_H_

#include <string>
#include <string_view>

namespace torfox::utf8 {

// Strict UTF-8 decoding: overlong forms, surrogates, code points above
// U+10FFFF and truncated sequences each become one |replacement|.
std::u32string Decode(std::string_view text, char32_t replacement);

// Encodes code points; surrogates and values above U+10FFFF become U+FFFD.
std::string Encode(std::u32string_view text);

// True if |text| is well-formed UTF-8.
bool IsValid(std::string_view text);

}  // namespace torfox::utf8

#endif  // TORFOX_CORE_UTIL_UTF8_H_
