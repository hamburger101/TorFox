// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UPDATE_MINISIGN_H_
#define TORFOX_CORE_UPDATE_MINISIGN_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace torfox::update {

// A minisign public key: "RW..." (base64 of "Ed", an 8-byte key id and the
// 32-byte Ed25519 public key).
struct PublicKey {
  std::array<uint8_t, 8> key_id{};
  std::array<uint8_t, 32> key{};
};

std::optional<PublicKey> ParsePublicKey(std::string_view text);

struct SignatureCheck {
  bool ok = false;
  std::string error;            // Why it failed, for the log and the user.
  std::string trusted_comment;  // Set only if |ok|.
};

inline constexpr std::size_t kMaxSignatureFileBytes = 4096;

// Verifies a minisign signature file (the four lines of <file>.minisig) over
// |data| with libsodium. Only prehashed signatures ("ED": Ed25519 over
// BLAKE2b-512 of the data, what minisign makes by default) are accepted.
// The key id must be |key|'s, the signature must verify, and so must the
// global signature over the trusted comment, which is returned.
SignatureCheck VerifyMinisign(const PublicKey& key, std::string_view signature_file,
                              std::string_view data);

}  // namespace torfox::update

#endif  // TORFOX_CORE_UPDATE_MINISIGN_H_
