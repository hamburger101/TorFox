// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/search.h"

#include "src/core/tor/onion.h"

namespace torfox::search {

std::string ForEngine(std::string_view search_url, std::string_view engine, bool tor) {
  constexpr std::string_view kSearch = "https://duckduckgo.com/?q=";
  if (search_url.substr(0, kSearch.size()) != kSearch) {
    return std::string(search_url);
  }
  const std::string_view query = search_url.substr(kSearch.size());
  std::string_view host = "duckduckgo.com";
  std::string_view path = "/";
  if (engine == "duckduckgo-html") {
    host = "html.duckduckgo.com";
    path = "/html/";
  } else if (engine == "duckduckgo-lite") {
    host = "lite.duckduckgo.com";
    path = "/lite/";
  }
  // Tor windows: DuckDuckGo's onion service, which serves all three pages.
  if (tor) {
    host = tor::kDuckDuckGoOnion;
  }
  std::string url = "https://";
  url += host;
  url += path;
  url += "?q=";
  url += query;
  return url;
}

}  // namespace torfox::search
