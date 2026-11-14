// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_CONTEXT_MENU_H_
#define TORFOX_CORE_BROWSING_CONTEXT_MENU_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace torfox::browsing {

// The DuckDuckGo URL for "Search DuckDuckGo for <selection>", or nothing if
// the selection must not be searched: empty, too long, or mentioning an onion
// address in any form (an onion address is never sent to a search engine).
std::optional<std::string> SelectionSearchUrl(std::string_view selection);

// A one-line excerpt for menu labels: runs of whitespace become one space,
// and text longer than |max_chars| code points is cut at a character
// boundary and ends with an ellipsis.
std::string MenuExcerpt(std::string_view text, std::size_t max_chars);

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_CONTEXT_MENU_H_
