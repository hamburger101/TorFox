// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SCHEME_RESOURCES_H_
#define TORFOX_BROWSER_SCHEME_RESOURCES_H_

#include <cstddef>
#include <string_view>

namespace torfox::resources {

// One file of the built UI (ui/dist), compiled into the binary by
// scripts/gen_resources.py. The table is read-only and is the only source of
// torfox:// content: there is no filesystem fallback.
struct Resource {
  std::string_view path;  // e.g. "pages/settings/index.html", "assets/x-1a2b.js"
  std::string_view mime;  // A bare MIME type, e.g. "text/html" (no parameters).
  const unsigned char* data;
  std::size_t size;
};

// Exact-match lookup. Returns nullptr if |path| is not in the table.
const Resource* Find(std::string_view path);

std::size_t Count();

}  // namespace torfox::resources

#endif  // TORFOX_BROWSER_SCHEME_RESOURCES_H_
