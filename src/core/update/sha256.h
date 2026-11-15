// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UPDATE_SHA256_H_
#define TORFOX_CORE_UPDATE_SHA256_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace torfox::update {

// SHA-256 (libsodium), fed in pieces: an update is hashed as it downloads.
class Sha256 {
 public:
  Sha256();
  void Update(std::string_view data);
  // Lower-case hex. The object is spent afterwards.
  std::string FinalHex();

 private:
  alignas(16) std::array<unsigned char, 128> state_{};  // crypto_hash_sha256_state.
  bool ready_ = false;
};

// The SHA-256 of a whole file and its size, read from disk now; nullopt if it
// cannot be read or is larger than |max_bytes|.
struct FileDigest {
  uint64_t size = 0;
  std::string sha256;
};
std::optional<FileDigest> HashFile(const std::filesystem::path& path, uint64_t max_bytes);

}  // namespace torfox::update

#endif  // TORFOX_CORE_UPDATE_SHA256_H_
