// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_URL_SEARCH_H_
#define TORFOX_CORE_URL_SEARCH_H_

#include <string>
#include <string_view>

namespace torfox::search {

// Settings > Search > Search engine. Every choice is a DuckDuckGo page:
//   "duckduckgo"       https://duckduckgo.com/?q=          (the default)
//   "duckduckgo-html"  https://html.duckduckgo.com/html/?q= (no JavaScript)
//   "duckduckgo-lite"  https://lite.duckduckgo.com/lite/?q= (text only)
// In Tor windows the same page on DuckDuckGo's onion service.
inline constexpr std::string_view kEngineSetting = "search.engine";

// |search_url| is a search made by DuckDuckGoSearchUrl
// ("https://duckduckgo.com/?q=<encoded query>"). Returns the same search for
// |engine| (an unknown engine gets the default page) in a Tor window if
// |tor|, else on the clearnet. Any other URL is returned unchanged.
std::string ForEngine(std::string_view search_url, std::string_view engine, bool tor);

}  // namespace torfox::search

#endif  // TORFOX_CORE_URL_SEARCH_H_
