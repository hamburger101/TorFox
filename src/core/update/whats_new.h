// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UPDATE_WHATS_NEW_H_
#define TORFOX_CORE_UPDATE_WHATS_NEW_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// "What's new": after TorFox has been updated, the first start opens a tab
// with the new version's release notes. The profile keeps a record of the
// version that last ran (the file "last-version").
namespace torfox::update {

inline constexpr std::size_t kMaxLastVersionBytes = 64;
// Bundled notes are never longer than release notes may be
// (scripts/release.py, docs/release-notes/README.md).
inline constexpr std::size_t kMaxBundledNotesBytes = 16 * 1024;

// The version a "last-version" record names, if it is one (surrounding
// spaces and line breaks are ignored).
std::optional<std::string> RecordedVersion(std::string_view record);

// True if the record names a version older than |current|. No record (a
// new profile), anything that is not a version, the same version or a newer
// one (TorFox was downgraded) is false: What's new is only for updates.
bool ShouldShowWhatsNew(std::string_view record, std::string_view current);

// Just before TorFox starts a checked update's setup it records the version
// it hands over (the file "update-handoff"). At the next start:
//   kInstalled:    the record names |current|: this version is the update
//                  TorFox checked against its signing key.
//   kNotInstalled: it names a newer version than |current|: the setup failed
//                  or did not run.
//   kNone:         no record, not a version, or an older version (TorFox was
//                  installed again by hand since).
enum class HandoffOutcome { kNone, kInstalled, kNotInstalled };
HandoffOutcome CheckHandoff(std::string_view record, std::string_view current);

// The notes as the What's new page shows them: Windows line breaks become
// "\n". Empty if they are not UTF-8 or are longer than allowed.
std::string NormalizeNotes(std::string_view raw);

}  // namespace torfox::update

#endif  // TORFOX_CORE_UPDATE_WHATS_NEW_H_
