// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/update/minisign.h"

#include <cstring>
#include <vector>

#include <sodium.h>

namespace torfox::update {

namespace {

bool SodiumReady() {
  // sodium_init() is thread-safe and may be called more than once; it
  // returns 1 if already initialised and -1 only on failure.
  static const bool ready = sodium_init() >= 0;
  return ready;
}

// Strict standard base64 (with padding); the whole input must decode to
// exactly |size| bytes.
bool DecodeBase64(std::string_view text, uint8_t* out, std::size_t size) {
  if (text.empty() || text.size() > 1024) {
    return false;
  }
  std::size_t written = 0;
  const char* end = nullptr;
  std::vector<uint8_t> buf(text.size());
  if (sodium_base642bin(buf.data(), buf.size(), text.data(), text.size(), nullptr, &written, &end,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      end != text.data() + text.size() || written != size) {
    return false;
  }
  std::memcpy(out, buf.data(), size);
  return true;
}

std::string_view StripCr(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

SignatureCheck Fail(std::string why) {
  SignatureCheck c;
  c.error = std::move(why);
  return c;
}

}  // namespace

std::optional<PublicKey> ParsePublicKey(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  uint8_t raw[42];
  if (!SodiumReady() || !DecodeBase64(text, raw, sizeof raw) || raw[0] != 'E' || raw[1] != 'd') {
    return std::nullopt;
  }
  PublicKey key;
  std::memcpy(key.key_id.data(), raw + 2, 8);
  std::memcpy(key.key.data(), raw + 10, 32);
  return key;
}

SignatureCheck VerifyMinisign(const PublicKey& key, std::string_view signature_file,
                              std::string_view data) {
  if (!SodiumReady()) {
    return Fail("the signature library could not start");
  }
  if (signature_file.size() > kMaxSignatureFileBytes) {
    return Fail("the signature file is too large");
  }
  // Four lines: untrusted comment, signature, trusted comment, global
  // signature. Anything after them must be empty.
  std::string_view lines[4];
  std::string_view rest = signature_file;
  for (std::string_view& line : lines) {
    const std::size_t nl = rest.find('\n');
    if (nl == std::string_view::npos) {
      line = StripCr(rest);
      rest = {};
    } else {
      line = StripCr(rest.substr(0, nl));
      rest.remove_prefix(nl + 1);
    }
  }
  if (rest.find_first_not_of("\r\n") != std::string_view::npos) {
    return Fail("the signature file has extra content");
  }
  constexpr std::string_view kUntrusted = "untrusted comment: ";
  constexpr std::string_view kTrusted = "trusted comment: ";
  if (lines[0].substr(0, kUntrusted.size()) != kUntrusted) {
    return Fail("not a minisign signature");
  }
  uint8_t sig[74];
  if (!DecodeBase64(lines[1], sig, sizeof sig)) {
    return Fail("the signature is malformed");
  }
  if (sig[0] != 'E' || sig[1] != 'D') {
    return Fail("the signature is not a prehashed (ED) minisign signature");
  }
  if (std::memcmp(sig + 2, key.key_id.data(), 8) != 0) {
    return Fail("the signature was made with a different key");
  }
  uint8_t digest[crypto_generichash_BYTES_MAX];
  static_assert(sizeof digest == 64);
  if (crypto_generichash(digest, sizeof digest, reinterpret_cast<const unsigned char*>(data.data()),
                         data.size(), nullptr, 0) != 0 ||
      crypto_sign_verify_detached(sig + 10, digest, sizeof digest, key.key.data()) != 0) {
    return Fail("the signature does not match");
  }
  if (lines[2].substr(0, kTrusted.size()) != kTrusted) {
    return Fail("the trusted comment is missing");
  }
  const std::string_view trusted = lines[2].substr(kTrusted.size());
  uint8_t global[64];
  if (!DecodeBase64(lines[3], global, sizeof global)) {
    return Fail("the trusted comment signature is malformed");
  }
  std::vector<uint8_t> signed_comment(sig + 10, sig + 74);
  signed_comment.insert(signed_comment.end(), trusted.begin(), trusted.end());
  if (crypto_sign_verify_detached(global, signed_comment.data(), signed_comment.size(),
                                  key.key.data()) != 0) {
    return Fail("the trusted comment signature does not match");
  }
  SignatureCheck ok;
  ok.ok = true;
  ok.trusted_comment = std::string(trusted);
  return ok;
}

}  // namespace torfox::update
