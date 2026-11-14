// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/tor/onion.h"

#include <cctype>

#include "src/core/url/guards.h"

namespace torfox::tor {

namespace {

// Keccak-f[1600], as in FIPS 202 (lane order and constants from the
// specification's reference description).
constexpr uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};
constexpr int kRotations[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr int kPiLanes[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                              15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

uint64_t Rotl(uint64_t x, int n) {
  return (x << n) | (x >> (64 - n));
}

void KeccakF(uint64_t s[25]) {
  for (int round = 0; round < 24; ++round) {
    uint64_t c[5];
    for (int x = 0; x < 5; ++x) {
      c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
    }
    for (int x = 0; x < 5; ++x) {
      const uint64_t d = c[(x + 4) % 5] ^ Rotl(c[(x + 1) % 5], 1);
      for (int y = 0; y < 25; y += 5) {
        s[y + x] ^= d;
      }
    }
    uint64_t t = s[1];
    for (int i = 0; i < 24; ++i) {
      const int j = kPiLanes[i];
      const uint64_t next = s[j];
      s[j] = Rotl(t, kRotations[i]);
      t = next;
    }
    for (int y = 0; y < 25; y += 5) {
      uint64_t row[5];
      for (int x = 0; x < 5; ++x) {
        row[x] = s[y + x];
      }
      for (int x = 0; x < 5; ++x) {
        s[y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
      }
    }
    s[0] ^= kRoundConstants[round];
  }
}

int Base32Value(char c) {
  if (c >= 'a' && c <= 'z') {
    return c - 'a';
  }
  if (c >= '2' && c <= '7') {
    return 26 + (c - '2');
  }
  return -1;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::array<uint8_t, 32> Sha3_256(std::string_view data) {
  constexpr std::size_t kRate = 136;  // (1600 - 2 * 256) / 8
  uint64_t s[25] = {};
  std::size_t pos = 0;
  for (unsigned char byte : data) {
    s[pos / 8] ^= static_cast<uint64_t>(byte) << (8 * (pos % 8));
    if (++pos == kRate) {
      KeccakF(s);
      pos = 0;
    }
  }
  // SHA-3 domain separation (01) and pad10*1.
  s[pos / 8] ^= static_cast<uint64_t>(0x06) << (8 * (pos % 8));
  s[(kRate - 1) / 8] ^= static_cast<uint64_t>(0x80) << (8 * ((kRate - 1) % 8));
  KeccakF(s);
  std::array<uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>(s[i / 8] >> (8 * (i % 8)));
  }
  return out;
}

OnionCheck CheckOnionHost(std::string_view host) {
  if (!host.empty() && host.back() == '.') {
    host.remove_suffix(1);
  }
  constexpr std::string_view kSuffix = ".onion";
  if (host.size() <= kSuffix.size() || host.substr(host.size() - kSuffix.size()) != kSuffix) {
    return OnionCheck::kNotOnion;
  }
  host.remove_suffix(kSuffix.size());
  const std::size_t dot = host.rfind('.');
  const std::string_view label = dot == std::string_view::npos ? host : host.substr(dot + 1);
  if (label.size() == 16) {
    return OnionCheck::kOldVersion;
  }
  if (label.size() != 56) {
    return OnionCheck::kBadLength;
  }
  // 56 base32 characters are 280 bits: the 32-byte public key, a 2-byte
  // checksum and the version byte.
  uint8_t bytes[35] = {};
  uint32_t buffer = 0;
  int bits = 0;
  std::size_t n = 0;
  for (char c : label) {
    const int v = Base32Value(c);
    if (v < 0) {
      return OnionCheck::kBadCharacters;
    }
    buffer = (buffer << 5) | static_cast<uint32_t>(v);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      bytes[n++] = static_cast<uint8_t>(buffer >> bits);
      buffer &= (1u << bits) - 1;
    }
  }
  if (n != sizeof(bytes) || bytes[34] != 3) {
    return OnionCheck::kBadVersion;
  }
  std::string input = ".onion checksum";
  input.append(reinterpret_cast<const char*>(bytes), 32);
  input.push_back(static_cast<char>(3));
  const std::array<uint8_t, 32> digest = Sha3_256(input);
  if (digest[0] != bytes[32] || digest[1] != bytes[33]) {
    return OnionCheck::kBadChecksum;
  }
  return OnionCheck::kValid;
}

std::string_view OnionCheckMessage(OnionCheck check) {
  switch (check) {
    case OnionCheck::kValid:
      return "";
    case OnionCheck::kNotOnion:
      return "This is not an onion address.";
    case OnionCheck::kOldVersion:
      return "This is an old (version 2) onion address. Tor no longer supports them; ask the site "
             "for its new address.";
    case OnionCheck::kBadLength:
      return "This onion address has the wrong length: onion addresses have 56 characters before "
             "\".onion\".";
    case OnionCheck::kBadCharacters:
      return "This onion address contains characters onion addresses never have (only a-z and "
             "2-7 are used).";
    case OnionCheck::kBadVersion:
      return "This onion address is not valid.";
    case OnionCheck::kBadChecksum:
      return "This onion address has a typing mistake: check it and try again.";
  }
  return "This onion address is not valid.";
}

OnionInput OnionUrlFromInput(std::string_view raw) {
  OnionInput out;
  const std::string_view input = Trim(raw);
  for (char c : input) {
    if (std::isspace(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) < 0x20) {
      out.error = "Onion addresses are never searched for. Enter just the address, for example "
                  "http://" + std::string(kDuckDuckGoOnion) + "/";
      return out;
    }
  }
  std::string url;
  if (input.find("://") != std::string_view::npos) {
    if (!StartsWithNoCase(input, "http://") && !StartsWithNoCase(input, "https://")) {
      out.error = "Onion sites can only be opened with http:// or https://.";
      return out;
    }
    url = std::string(input);
  } else {
    url = "http://" + std::string(input);
  }
  const std::string host = guards::HostOf(url);
  const OnionCheck check = CheckOnionHost(host);
  if (check != OnionCheck::kValid) {
    out.error = std::string(OnionCheckMessage(check));
    return out;
  }
  out.url = std::move(url);
  return out;
}

}  // namespace torfox::tor
