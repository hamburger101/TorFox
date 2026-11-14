// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UTIL_FILE_IO_H_
#define TORFOX_CORE_UTIL_FILE_IO_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace torfox {

enum class ReadStatus { kOk, kMissing, kTooLarge, kError };

struct ReadResult {
  ReadStatus status = ReadStatus::kError;
  std::string data;
};

// Reads a whole file, refusing files larger than |max_bytes|.
ReadResult ReadFileLimited(const std::filesystem::path& path, std::size_t max_bytes);

// Writes |data| to "<path>.tmp", flushes it to disk, then atomically replaces
// |path|. On failure the previous contents of |path| are left untouched and
// false is returned. Parent directories are created if needed. On POSIX the
// file is created with mode 0600.
bool WriteFileAtomically(const std::filesystem::path& path, std::string_view data);

// Renames |from| to |to|, replacing |to| if it exists. Returns false on error.
bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace torfox

#endif  // TORFOX_CORE_UTIL_FILE_IO_H_
