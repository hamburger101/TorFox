// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/update/sha256.h"

#include <fstream>
#include <vector>

#include <sodium.h>

namespace torfox::update {

static_assert(sizeof(crypto_hash_sha256_state) <= 128);

namespace {

crypto_hash_sha256_state* State(std::array<unsigned char, 128>& bytes) {
  return reinterpret_cast<crypto_hash_sha256_state*>(bytes.data());
}

}  // namespace

Sha256::Sha256() {
  ready_ = sodium_init() >= 0 && crypto_hash_sha256_init(State(state_)) == 0;
}

void Sha256::Update(std::string_view data) {
  if (ready_ && crypto_hash_sha256_update(State(state_),
                                          reinterpret_cast<const unsigned char*>(data.data()),
                                          data.size()) != 0) {
    ready_ = false;
  }
}

std::string Sha256::FinalHex() {
  unsigned char digest[crypto_hash_sha256_BYTES];
  if (!ready_ || crypto_hash_sha256_final(State(state_), digest) != 0) {
    ready_ = false;
    return {};  // Never equal to a 64-digit hash.
  }
  ready_ = false;
  char hex[2 * crypto_hash_sha256_BYTES + 1];
  sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
  return hex;
}

std::optional<FileDigest> HashFile(const std::filesystem::path& path, uint64_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  Sha256 hash;
  FileDigest d;
  std::vector<char> buf(1 << 16);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::streamsize n = in.gcount();
    if (n <= 0) {
      break;
    }
    d.size += static_cast<uint64_t>(n);
    if (d.size > max_bytes) {
      return std::nullopt;
    }
    hash.Update(std::string_view(buf.data(), static_cast<std::size_t>(n)));
  }
  if (in.bad()) {
    return std::nullopt;
  }
  d.sha256 = hash.FinalHex();
  if (d.sha256.empty()) {
    return std::nullopt;
  }
  return d;
}

}  // namespace torfox::update
