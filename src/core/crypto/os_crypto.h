// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_CRYPTO_OS_CRYPTO_H_
#define TORFOX_CORE_CRYPTO_OS_CRYPTO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The few primitives TorFox needs before it has a crypto library of its own,
// taken from the operating system rather than written here: Windows CNG
// (bcrypt) and Apple's CommonCrypto / Security framework.
namespace torfox::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

// HMAC-SHA-256 of |data| under |key|; nullopt only if the OS call fails.
std::optional<Sha256Digest> HmacSha256(std::string_view key, std::string_view data);

// |n| bytes from the OS's cryptographic random generator; "" on failure.
std::string RandomBytes(std::size_t n);

// Compares without an early exit, so the time taken does not reveal where
// two secrets first differ.
bool ConstantTimeEqual(std::string_view a, std::string_view b);

// Upper-case hex, and its inverse (nullopt for odd lengths or non-hex).
std::string HexEncode(std::string_view bytes);
std::optional<std::string> HexDecode(std::string_view hex);

}  // namespace torfox::crypto

#endif  // TORFOX_CORE_CRYPTO_OS_CRYPTO_H_
