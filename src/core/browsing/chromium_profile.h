// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_
#define TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_

#include <filesystem>
#include <string_view>

namespace torfox::chromium_profile {

// Chromium records every download of a profile in its "History" database
// (the file's path and the URLs it came from), whatever the profile's
// history settings, and the latest download's details in "DownloadMetadata".
// TorFox keeps its downloads list in memory only, so it erases these records
// itself while Chromium is not running: before CEF starts and after it shuts
// down. Run at start, it also covers a session that crashed.
struct ScrubResult {
  bool ok = true;           // False if the database exists but could not be cleaned.
  int rows = 0;             // Download rows deleted.
  bool metadata_removed = false;
};

// |profile_dir| is a Chromium profile folder (for TorFox, ".../User Data/Normal").
// Deleted rows are overwritten (secure_delete) and the file is compacted.
// Never creates anything; a missing database is fine.
ScrubResult ScrubDownloadRecords(const std::filesystem::path& profile_dir);

struct SiteRecordsResult {
  bool ok = true;  // False if Preferences exists but could not be read or rewritten.
  int removed = 0;  // Record sets erased.
};

// Chromium keeps per-site scores in the profile's Preferences file
// ("site engagement" and "media engagement": every site visited in normal
// windows), which TorFox never uses or shows, and per-site zoom levels. This
// erases the scores, and the zoom levels too if |zoom_levels| (they go with
// the history). Everything else in the file is kept. Run only while Chromium
// is not using the profile.
SiteRecordsResult ScrubSiteRecords(const std::filesystem::path& profile_dir, bool zoom_levels);

// Sets the profile's "proxy" preference to {"mode": |mode|} ("direct" or
// "system") in its Preferences file, creating the file and folder for a
// new profile. Chromium builds a profile's network context from this
// before TorFox can set any preference at run time, so this is the only
// way to keep it from looking for a proxy (WPAD) as it starts. Run only
// while Chromium is not using the profile. False if it could not be written
// (an unreadable file is left alone).
bool WriteProxyMode(const std::filesystem::path& profile_dir, std::string_view mode);

}  // namespace torfox::chromium_profile

#endif  // TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_
