// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/privacy/clear_data.h"

#include <system_error>

namespace torfox::clear_data {

namespace {

// Chromium 156's profile layout. A path that does not exist is skipped, so
// an entry for a file Chromium no longer writes costs nothing.
constexpr const char* kHistory[] = {
    "History", "History-journal", "Visited Links", "Top Sites", "Top Sites-journal",
    "Favicons", "Favicons-journal", "Shortcuts", "Shortcuts-journal",
    "Network Action Predictor", "Network Action Predictor-journal",
};
constexpr const char* kCookies[] = {
    "Network/Cookies", "Network/Cookies-journal",
    "Network/Trust Tokens", "Network/Trust Tokens-journal",
    "Network/Reporting and NEL", "Network/Reporting and NEL-journal",
    "Local Storage", "Session Storage", "IndexedDB", "Service Worker", "File System",
    "WebStorage", "databases", "shared_proto_db", "Shared Storage", "Shared Storage-wal",
    "blob_storage", "SharedStorage", "Storage",
};
constexpr const char* kCache[] = {
    "Cache", "Code Cache", "GPUCache", "DawnGraphiteCache", "DawnWebGPUCache",
    "Shared Dictionary", "Network/Network Persistent State",
};

void AddAll(std::vector<std::filesystem::path>& out, const auto& list) {
  for (const char* p : list) {
    out.emplace_back(std::filesystem::path(p).make_preferred());
  }
}

}  // namespace

std::vector<std::filesystem::path> PathsFor(const Categories& c) {
  std::vector<std::filesystem::path> out;
  if (c.history) {
    AddAll(out, kHistory);
  }
  if (c.cookies) {
    AddAll(out, kCookies);
  }
  if (c.cache) {
    AddAll(out, kCache);
  }
  return out;
}

Result Clear(const std::filesystem::path& profile_dir, const Categories& c) {
  Result r;
  for (const std::filesystem::path& rel : PathsFor(c)) {
    const std::filesystem::path p = profile_dir / rel;
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::symlink_status(p, ec);
    if (ec || !std::filesystem::exists(st)) {
      continue;
    }
    // remove_all removes a link itself and never follows it.
    std::filesystem::remove_all(p, ec);
    if (ec || std::filesystem::exists(std::filesystem::symlink_status(p, ec))) {
      ++r.failed;
    } else {
      ++r.removed;
    }
  }
  return r;
}

}  // namespace torfox::clear_data
