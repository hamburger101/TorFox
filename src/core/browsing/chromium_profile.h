// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_
#define TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_

#include <filesystem>

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

}  // namespace torfox::chromium_profile

#endif  // TORFOX_CORE_BROWSING_CHROMIUM_PROFILE_H_
