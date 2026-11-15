// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_PRIVACY_CLEAR_DATA_H_
#define TORFOX_CORE_PRIVACY_CLEAR_DATA_H_

#include <filesystem>
#include <vector>

namespace torfox::clear_data {

// Settings > Privacy & Security > Clear when TorFox closes.
struct Categories {
  bool history = false;  // Chromium's per-site records of visits (TorFox's own history is separate).
  bool cookies = false;  // Cookies and site data (storage, databases, service workers).
  bool cache = false;    // Caches, and the network state that records visited hosts.
  bool any() const { return history || cookies || cache; }
};

// The files and folders of a Chromium profile that hold |c|, relative to
// the profile folder. Always kept: preferences, and the HSTS list
// (Network/TransportSecurity), which makes later connections safer but could
// in principle help a site recognise a returning browser.
std::vector<std::filesystem::path> PathsFor(const Categories& c);

struct Result {
  int removed = 0;  // Paths that existed and are gone.
  int failed = 0;   // Paths that could not be deleted.
};

// Deletes PathsFor(|c|) under |profile_dir|. Links are removed, never
// followed. Run only while Chromium is not using the profile.
Result Clear(const std::filesystem::path& profile_dir, const Categories& c);

}  // namespace torfox::clear_data

#endif  // TORFOX_CORE_PRIVACY_CLEAR_DATA_H_
