// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/crypto/os_crypto.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#include <Security/SecRandom.h>
#endif

namespace torfox::crypto {

std::optional<Sha256Digest> HmacSha256(std::string_view key, std::string_view data) {
  Sha256Digest out{};
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                  BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
    return std::nullopt;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCRYPT_SUCCESS(BCryptCreateHash(
      alg, &hash, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0));
  ok = ok && BCRYPT_SUCCESS(BCryptHashData(
                 hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                 static_cast<ULONG>(data.size()), 0));
  ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0));
  if (hash) {
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) {
    return std::nullopt;
  }
#elif defined(__APPLE__)
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data.data(), data.size(), out.data());
#else
#error "No HMAC-SHA-256 implementation for this platform"
#endif
  return out;
}

std::string RandomBytes(std::size_t n) {
  std::string out(n, '\0');
  if (n == 0) {
    return out;
  }
#if defined(_WIN32)
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                      static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return std::string();
  }
#elif defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, n, out.data()) != errSecSuccess) {
    return std::string();
  }
#endif
  return out;
}

bool ConstantTimeEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string HexEncode(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (char c : bytes) {
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back(kDigits[u >> 4]);
    out.push_back(kDigits[u & 0x0F]);
  }
  return out;
}

std::optional<std::string> HexDecode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return std::nullopt;
  }
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = value(hex[i]);
    const int lo = value(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

}  // namespace torfox::crypto
