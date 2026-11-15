// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The updater's signature check and release rules. The minisign vectors in
// data/update were made by the official minisign tool (see its README); the
// signer below exists only so the manifest rules can be tested with content
// the vectors do not cover, and is never part of TorFox.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sodium.h>

#include "gtest/gtest.h"
#include "src/core/json.h"
#include "src/core/update/minisign.h"
#include "src/core/update/release.h"
#include "src/core/update/sha256.h"
#include "tests/unit/test_util.h"

namespace torfox::update {
namespace {

std::string ReadVector(const char* name) {
  std::ifstream in(std::filesystem::path(TORFOX_UNIT_DATA_DIR) / "update" / name,
                   std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// The key line of a minisign .pub file.
PublicKey VectorKey(const char* name) {
  std::string text = ReadVector(name);
  const std::size_t nl = text.find('\n');
  std::optional<PublicKey> key = ParsePublicKey(text.substr(nl + 1));
  EXPECT_TRUE(key) << name;
  return key.value_or(PublicKey{});
}

std::string Base64(const uint8_t* data, std::size_t size) {
  std::string out(sodium_base64_ENCODED_LEN(size, sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), data, size, sodium_base64_VARIANT_ORIGINAL);
  out.resize(std::strlen(out.c_str()));
  return out;
}

// A test key pair, and minisign's prehashed ("ED") format.
struct TestSigner {
  explicit TestSigner(uint8_t seed_byte) {
    EXPECT_GE(sodium_init(), 0);
    std::array<uint8_t, 32> seed;
    seed.fill(seed_byte);
    crypto_sign_seed_keypair(public_key.key.data(), secret.data(), seed.data());
    public_key.key_id.fill(static_cast<uint8_t>(seed_byte ^ 0x5A));
  }

  std::string Sign(std::string_view data, std::string_view trusted_comment) const {
    uint8_t digest[64];
    crypto_generichash(digest, sizeof digest, reinterpret_cast<const uint8_t*>(data.data()),
                       data.size(), nullptr, 0);
    uint8_t sig[74] = {'E', 'D'};
    std::memcpy(sig + 2, public_key.key_id.data(), 8);
    crypto_sign_detached(sig + 10, nullptr, digest, sizeof digest, secret.data());
    std::string signed_comment(reinterpret_cast<const char*>(sig + 10), 64);
    signed_comment += trusted_comment;
    uint8_t global[64];
    crypto_sign_detached(global, nullptr, reinterpret_cast<const uint8_t*>(signed_comment.data()),
                         signed_comment.size(), secret.data());
    return "untrusted comment: signature from a test key\n" + Base64(sig, sizeof sig) +
           "\ntrusted comment: " + std::string(trusted_comment) + "\n" +
           Base64(global, sizeof global) + "\n";
  }

  PublicKey public_key;
  std::array<uint8_t, 64> secret{};
};

Json ValidManifest() {
  Json assets = Json::object();
  assets["windows-x64"] = {{"name", "TorFox-2.0.1-win-x64-setup.exe"},
                           {"size", 1000},
                           {"sha256", std::string(64, 'a')}};
  assets["macos-arm64"] = {{"name", "TorFox-2.0.1-mac-arm64.dmg"},
                           {"size", 2000},
                           {"sha256", std::string(64, 'b')}};
  return {{"format", 1},     {"product", "TorFox"},          {"version", "2.0.1"},
          {"timestamp", 1800000000}, {"notes", "Release notes."}, {"assets", assets}};
}

ManifestCheck SignAndCheck(const Json& manifest, std::string comment = {}) {
  static const TestSigner signer(7);
  const std::string text = DumpJson(manifest);
  if (comment.empty()) {
    const std::optional<Version> v =
        ParseVersion(GetString(manifest, "version").value_or("0.0.0"));
    comment = TrustedComment(v.value_or(Version{}), GetInt(manifest, "timestamp").value_or(0));
  }
  return CheckManifest(signer.public_key, text, signer.Sign(text, comment));
}

// ----- minisign, against the official tool's output -------------------------------

TEST(Minisign, RealMinisignSignaturesVerify) {
  const SignatureCheck c =
      VerifyMinisign(VectorKey("a.pub"), ReadVector("good.minisig"), ReadVector("manifest.json"));
  ASSERT_TRUE(c.ok) << c.error;
  EXPECT_EQ(c.trusted_comment, "torfox-release version:1.2.3 timestamp:1790000000");
}

TEST(Minisign, OtherKeysAreRefused) {
  const std::string manifest = ReadVector("manifest.json");
  EXPECT_FALSE(VerifyMinisign(VectorKey("a.pub"), ReadVector("wrong_key.minisig"), manifest).ok);
  EXPECT_FALSE(VerifyMinisign(VectorKey("b.pub"), ReadVector("good.minisig"), manifest).ok);
  // A key that claims A's key id but is B's key: the signature itself must
  // fail, not only the key id comparison.
  PublicKey forged = VectorKey("b.pub");
  forged.key_id = VectorKey("a.pub").key_id;
  const SignatureCheck c = VerifyMinisign(forged, ReadVector("good.minisig"), manifest);
  EXPECT_FALSE(c.ok);
  EXPECT_EQ(c.error, "the signature does not match");
}

TEST(Minisign, LegacySignaturesAreRefused) {
  const SignatureCheck c = VerifyMinisign(VectorKey("a.pub"), ReadVector("legacy.minisig"),
                                          ReadVector("manifest.json"));
  EXPECT_FALSE(c.ok);
  EXPECT_NE(c.error.find("prehashed"), std::string::npos) << c.error;
}

TEST(Minisign, ChangedDataIsRefused) {
  const PublicKey key = VectorKey("a.pub");
  const std::string sig = ReadVector("good.minisig");
  std::string manifest = ReadVector("manifest.json");
  manifest[manifest.find("123456789")] = '9';
  EXPECT_FALSE(VerifyMinisign(key, sig, manifest).ok);
  EXPECT_FALSE(VerifyMinisign(key, sig, ReadVector("manifest.json") + "\n").ok);
  EXPECT_FALSE(VerifyMinisign(key, sig, "").ok);
}

TEST(Minisign, ChangedTrustedCommentIsRefused) {
  std::string sig = ReadVector("good.minisig");
  sig.replace(sig.find("version:1.2.3"), 13, "version:1.2.4");
  const SignatureCheck c = VerifyMinisign(VectorKey("a.pub"), sig, ReadVector("manifest.json"));
  EXPECT_FALSE(c.ok);
  EXPECT_EQ(c.error, "the trusted comment signature does not match");
}

TEST(Minisign, MalformedSignatureFilesAreRefused) {
  const PublicKey key = VectorKey("a.pub");
  const std::string manifest = ReadVector("manifest.json");
  const std::string good = ReadVector("good.minisig");
  std::vector<std::string> lines;
  std::istringstream in(good);
  for (std::string line; std::getline(in, line);) {
    ASSERT_EQ(line.find('\r'), std::string::npos) << "the vectors are stored with LF line endings";
    lines.push_back(line);
  }
  ASSERT_EQ(lines.size(), 4u);
  const auto join = [](const std::vector<std::string>& l) {
    std::string s;
    for (const std::string& x : l) {
      s += x + "\n";
    }
    return s;
  };
  ASSERT_TRUE(VerifyMinisign(key, join(lines), manifest).ok);
  // CRLF line endings are the same file.
  std::string crlf;
  for (const std::string& x : lines) {
    crlf += x + "\r\n";
  }
  EXPECT_TRUE(VerifyMinisign(key, crlf, manifest).ok) << VerifyMinisign(key, crlf, manifest).error;

  std::vector<std::string> bad;
  bad.push_back("");
  bad.push_back(join({lines[0], lines[1], lines[2]}));                    // No global signature.
  bad.push_back(join({lines[1], lines[2], lines[3], lines[0]}));          // Wrong order.
  bad.push_back(join({lines[0], lines[1], lines[2], lines[3], "extra"}));  // Extra content.
  bad.push_back(join({"comment: x", lines[1], lines[2], lines[3]}));      // Not minisign.
  bad.push_back(join({lines[0], lines[1] + "=", lines[2], lines[3]}));    // Bad base64.
  bad.push_back(join({lines[0], " " + lines[1], lines[2], lines[3]}));
  bad.push_back(join({lines[0], lines[1].substr(0, 40), lines[2], lines[3]}));
  bad.push_back(join({lines[0], lines[1], "untrusted comment: x", lines[3]}));
  bad.push_back(join({lines[0], lines[1], lines[2], lines[3].substr(4)}));
  bad.push_back(join({lines[0], lines[1], lines[2], lines[3]}) + std::string(5000, '\n'));
  for (const std::string& b : bad) {
    EXPECT_FALSE(VerifyMinisign(key, b, manifest).ok) << b;
  }
}

TEST(Minisign, PublicKeys) {
  const std::string pub = ReadVector("a.pub");
  const std::string line = pub.substr(pub.find('\n') + 1);
  EXPECT_TRUE(ParsePublicKey(line));
  EXPECT_FALSE(ParsePublicKey(""));
  EXPECT_FALSE(ParsePublicKey(pub));  // The whole file: only the key is configured.
  EXPECT_FALSE(ParsePublicKey(line.substr(0, 50)));
  EXPECT_FALSE(ParsePublicKey("x" + line));
  // A signature line is base64 too, but not a key.
  const std::string sig = ReadVector("good.minisig");
  const std::size_t start = sig.find('\n') + 1;
  EXPECT_FALSE(ParsePublicKey(sig.substr(start, sig.find('\n', start) - start)));
}

// ----- Versions ----------------------------------------------------------------------

TEST(Release, Versions) {
  ASSERT_TRUE(ParseVersion("1.2.3"));
  EXPECT_EQ(VersionString(*ParseVersion("0.10.0")), "0.10.0");
  for (const char* bad : {"", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.3-beta", "01.2.3", "1.02.3",
                          "1..3", " 1.2.3", "1.2.3 ", "100000.0.0", "-1.2.3", "1.2.x", "1.2.3\n"}) {
    EXPECT_FALSE(ParseVersion(bad)) << bad;
  }
  const auto cmp = [](const char* a, const char* b) {
    return CompareVersions(*ParseVersion(a), *ParseVersion(b));
  };
  EXPECT_GT(cmp("1.2.10", "1.2.9"), 0);
  EXPECT_GT(cmp("2.0.0", "1.99.99"), 0);
  EXPECT_GT(cmp("1.10.0", "1.9.9"), 0);
  EXPECT_LT(cmp("0.9.9", "1.0.0"), 0);
  EXPECT_EQ(cmp("1.2.3", "1.2.3"), 0);
}

// ----- The signed manifest -----------------------------------------------------------

TEST(Release, ManifestSignedByTheOfficialTool) {
  const ManifestCheck c =
      CheckManifest(VectorKey("a.pub"), ReadVector("manifest.json"), ReadVector("good.minisig"));
  ASSERT_TRUE(c.ok) << c.error;
  EXPECT_EQ(VersionString(c.release.version), "1.2.3");
  EXPECT_EQ(c.release.timestamp, 1790000000);
  EXPECT_EQ(c.release.notes, "Fixes and improvements.");
  ASSERT_EQ(c.release.assets.size(), 3u);
  EXPECT_EQ(c.release.assets.at("windows-x64").name, "TorFox-1.2.3-win-x64-setup.exe");
  EXPECT_EQ(c.release.assets.at("windows-x64").size, 123456789u);
  EXPECT_EQ(c.release.assets.at("macos-x64").name, "TorFox-1.2.3-mac-x64.dmg");
}

TEST(Release, ManifestNeedsTheRightKeyAndComment) {
  const std::string manifest = ReadVector("manifest.json");
  ManifestCheck c = CheckManifest(VectorKey("a.pub"), manifest, ReadVector("wrong_key.minisig"));
  EXPECT_FALSE(c.ok);
  EXPECT_NE(c.error.find("signature is not valid"), std::string::npos) << c.error;
  c = CheckManifest(VectorKey("a.pub"), manifest, ReadVector("legacy.minisig"));
  EXPECT_FALSE(c.ok);
  // Signed by the right key, but the signed comment names another release.
  c = CheckManifest(VectorKey("a.pub"), manifest, ReadVector("comment_mismatch.minisig"));
  EXPECT_FALSE(c.ok);
  EXPECT_EQ(c.error, "the signed comment does not match the release manifest");
}

TEST(Release, ManifestContentRules) {
  ASSERT_TRUE(SignAndCheck(ValidManifest()).ok) << SignAndCheck(ValidManifest()).error;

  // The comment must name this version and time.
  EXPECT_FALSE(SignAndCheck(ValidManifest(), "torfox-release version:2.0.1 timestamp:1").ok);
  EXPECT_FALSE(SignAndCheck(ValidManifest(), "torfox-release version:2.0.2 timestamp:1800000000").ok);
  EXPECT_FALSE(SignAndCheck(ValidManifest(), "anything").ok);

  const auto with = [](auto&& change) {
    Json m = ValidManifest();
    change(m);
    return SignAndCheck(m);
  };
  // Each of these is signed correctly, and still refused.
  EXPECT_FALSE(with([](Json& m) { m["format"] = 2; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["product"] = "Other"; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["version"] = "2.0"; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["timestamp"] = 0; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["timestamp"] = "1800000000"; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["notes"] = std::string(kMaxNotesBytes + 1, 'n'); }).ok);
  EXPECT_FALSE(with([](Json& m) { m.erase("assets"); }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["sha256"] = std::string(63, 'a'); }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["sha256"] = std::string(64, 'A'); }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["sha256"] = std::string(64, 'g'); }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["size"] = 0; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["size"] = -5; }).ok);
  EXPECT_FALSE(
      with([](Json& m) { m["assets"]["windows-x64"]["size"] = kMaxAssetBytes + 1; }).ok);
  EXPECT_FALSE(with([](Json& m) { m["assets"]["windows-x64"]["size"] = 18446744073709551615ull; }).ok);
  // File names: this version's, .exe or .dmg, nothing that leaves the folder.
  for (const char* name : {"TorFox-2.0.0-win-x64-setup.exe", "TorFox-2.0.1-win-x64-setup.msi",
                           "../TorFox-2.0.1-win-x64-setup.exe", "TorFox-2.0.1-..\\x.exe",
                           "TorFox-2.0.1-/x.exe", "TorFox-2.0.1-.exe", "Other-2.0.1-x.exe",
                           "TorFox-2.0.1-a b.exe", "TorFox-2.0.1-a..exe"}) {
    EXPECT_FALSE(with([&](Json& m) { m["assets"]["windows-x64"]["name"] = name; }).ok) << name;
  }
  // A platform this version does not know about is skipped, not an error.
  const ManifestCheck future = with([](Json& m) {
    m["assets"]["linux-riscv"] = {{"name", "x"}, {"size", 1}, {"sha256", "zz"}};
  });
  ASSERT_TRUE(future.ok) << future.error;
  EXPECT_EQ(future.release.assets.size(), 2u);
  // Too large to be a manifest.
  Json big = ValidManifest();
  big["notes"] = std::string(kMaxManifestBytes, 'n');
  EXPECT_FALSE(SignAndCheck(big).ok);
}

TEST(Release, OnlyNewerReleasesAreOffered) {
  const ManifestCheck c = SignAndCheck(ValidManifest());  // 2.0.1, time 1800000000.
  ASSERT_TRUE(c.ok);
  const Release& r = c.release;
  Offer o = Decide(r, *ParseVersion("2.0.0"), 1700000000, "windows-x64");
  ASSERT_TRUE(o.update) << o.reason;
  EXPECT_EQ(o.asset.name, "TorFox-2.0.1-win-x64-setup.exe");
  EXPECT_TRUE(Decide(r, *ParseVersion("1.9.9"), 0, "macos-arm64").update);
  // Same, older: never (no downgrades).
  EXPECT_FALSE(Decide(r, *ParseVersion("2.0.1"), 0, "windows-x64").update);
  EXPECT_FALSE(Decide(r, *ParseVersion("2.1.0"), 0, "windows-x64").update);
  EXPECT_FALSE(Decide(r, *ParseVersion("10.0.0"), 0, "windows-x64").update);
  // The same release seen again (not installed yet) is still offered; an
  // older signed manifest replayed after a newer one was seen is not.
  EXPECT_TRUE(Decide(r, *ParseVersion("2.0.0"), 1800000000, "windows-x64").update);
  EXPECT_FALSE(Decide(r, *ParseVersion("2.0.0"), 1800000000, "windows-x64").replayed);
  EXPECT_FALSE(Decide(r, *ParseVersion("2.0.0"), 1800000001, "windows-x64").update);
  EXPECT_FALSE(Decide(r, *ParseVersion("2.0.0"), 1900000000, "windows-x64").update);
  EXPECT_TRUE(Decide(r, *ParseVersion("2.0.0"), 1800000001, "windows-x64").replayed);
  // A replay is reported as one even when its version is not newer.
  EXPECT_TRUE(Decide(r, *ParseVersion("2.0.1"), 1900000000, "windows-x64").replayed);
  EXPECT_TRUE(Decide(r, *ParseVersion("3.0.0"), 1900000000, "windows-x64").replayed);
  EXPECT_FALSE(Decide(r, *ParseVersion("3.0.0"), 1800000000, "windows-x64").replayed);
  // No file for this computer.
  EXPECT_FALSE(Decide(r, *ParseVersion("2.0.0"), 0, "macos-x64").update);
}

// ----- GitHub Releases ---------------------------------------------------------------

TEST(Release, Repositories) {
  EXPECT_TRUE(IsValidRepository("hamburger101/TorFox"));
  EXPECT_TRUE(IsValidRepository("a-b/c.d_e"));
  for (const char* bad : {"", "TorFox", "/TorFox", "a/", "a/b/c", "a/../b", "../a", "a b/c",
                          "a/b?x", "a/b#x", "a%2fb/c", "a\\b/c"}) {
    EXPECT_FALSE(IsValidRepository(bad)) << bad;
  }
  EXPECT_EQ(LatestReleaseUrl(Endpoints{}, "hamburger101/TorFox"),
            "https://api.github.com/repos/hamburger101/TorFox/releases/latest");
}

TEST(Release, OnlyGitHubsHostsAreAllowed) {
  const Endpoints e;
  for (const char* ok : {"https://api.github.com/repos/a/b/releases/latest",
                         "https://github.com/a/b/releases/download/v1/x",
                         "https://objects.githubusercontent.com/x?sig=1",
                         "https://release-assets.githubusercontent.com/x", "https://github.com"}) {
    EXPECT_TRUE(IsAllowedUrl(e, ok)) << ok;
  }
  for (const char* bad : {"http://github.com/x", "https://github.com.evil.example/x",
                          "https://evil.example/github.com", "https://evil.example/?https://github.com",
                          "https://user@github.com/x", "https://github.com@evil.example/x",
                          "https://github.com:8443/x", "https://GITHUB.com/x",
                          "https://github.com\\@evil.example/x", "https://github.com/a b",
                          "https://gist.github.com/x", "ftp://github.com/x", "github.com/x",
                          "javascript:alert(1)//https://github.com", "https://%67ithub.com/x", "",
                          "https://xn--github-xyz.com/x", "https://github.com./x"}) {
    EXPECT_FALSE(IsAllowedUrl(e, bad)) << bad;
  }
  // A test build's local server, only when configured.
  Endpoints local;
  local.allowed_origins = {"http://127.0.0.1:8123"};
  EXPECT_TRUE(IsAllowedUrl(local, "http://127.0.0.1:8123/x"));
  EXPECT_FALSE(IsAllowedUrl(local, "http://127.0.0.1:8124/x"));
  EXPECT_FALSE(IsAllowedUrl(local, "https://github.com/x"));
}

std::string ReleaseJson(const std::string& tag, const Json& assets, bool draft = false) {
  return DumpJson({{"tag_name", tag}, {"draft", draft}, {"prerelease", false}, {"assets", assets}});
}

Json AssetEntry(const std::string& name, const std::string& url) {
  return {{"name", name}, {"browser_download_url", url}, {"size", 1}};
}

TEST(Release, LatestReleaseAnswer) {
  const Endpoints e;
  const std::string base = "https://github.com/hamburger101/TorFox/releases/download/v2.0.1/";
  Json assets = Json::array({AssetEntry("manifest.json", base + "manifest.json"),
                             AssetEntry("manifest.json.minisig", base + "manifest.json.minisig"),
                             AssetEntry("TorFox-2.0.1-win-x64-setup.exe",
                                        base + "TorFox-2.0.1-win-x64-setup.exe")});
  std::string error;
  std::optional<LatestRelease> r =
      ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", assets), &error);
  ASSERT_TRUE(r) << error;
  EXPECT_EQ(r->manifest_url, base + "manifest.json");
  EXPECT_EQ(r->signature_url, base + "manifest.json.minisig");
  EXPECT_EQ(r->asset_urls.at("TorFox-2.0.1-win-x64-setup.exe"),
            base + "TorFox-2.0.1-win-x64-setup.exe");

  // Files of another repository, another tag or another host are ignored.
  Json foreign = Json::array(
      {AssetEntry("manifest.json",
                  "https://github.com/someone/TorFox/releases/download/v2.0.1/manifest.json"),
       AssetEntry("manifest.json.minisig", base + "manifest.json.minisig")});
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", foreign), &error));
  Json other_tag = Json::array(
      {AssetEntry("manifest.json",
                  "https://github.com/hamburger101/TorFox/releases/download/v1.0.0/manifest.json"),
       AssetEntry("manifest.json.minisig", base + "manifest.json.minisig")});
  EXPECT_FALSE(
      ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", other_tag), &error));
  Json renamed = Json::array({AssetEntry("manifest.json", base + "other.json"),
                              AssetEntry("manifest.json.minisig", base + "manifest.json.minisig")});
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", renamed), &error));

  // No signature: nothing to check, so nothing to offer.
  Json unsigned_assets = Json::array({AssetEntry("manifest.json", base + "manifest.json")});
  EXPECT_FALSE(
      ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", unsigned_assets), &error));
  EXPECT_EQ(error, "the latest release has no signed manifest");

  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("v2.0.1", assets, true),
                                  &error));
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("../v2", assets), &error));
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", ReleaseJson("", assets), &error));
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", "not json", &error));
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox", "[]", &error));
  EXPECT_FALSE(ParseLatestRelease(e, "hamburger101/TorFox",
                                  std::string(kMaxReleaseJsonBytes + 1, ' '), &error));
}

TEST(Release, SavedState) {
  SavedState s{1700000000, 1790000000};
  const SavedState back = ParseSavedState(SerializeSavedState(s));
  EXPECT_EQ(back.last_check, 1700000000);
  EXPECT_EQ(back.newest_seen, 1790000000);
  for (const char* bad : {"", "{", "[]", "{\"format\":2,\"lastCheck\":5,\"newestSeen\":5}",
                          "{\"lastCheck\":5,\"newestSeen\":5}"}) {
    const SavedState d = ParseSavedState(bad);
    EXPECT_EQ(d.last_check, 0) << bad;
    EXPECT_EQ(d.newest_seen, 0) << bad;
  }
  const SavedState neg = ParseSavedState("{\"format\":1,\"lastCheck\":-5,\"newestSeen\":\"9\"}");
  EXPECT_EQ(neg.last_check, 0);
  EXPECT_EQ(neg.newest_seen, 0);
}

// ----- SHA-256 -----------------------------------------------------------------------

TEST(Sha256, KnownAnswers) {
  // FIPS 180-2 examples.
  Sha256 abc;
  abc.Update("abc");
  EXPECT_EQ(abc.FinalHex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Sha256 pieces;
  pieces.Update("abcdbcdecdefdefgefghfghighij");
  pieces.Update("hijkijkljklmklmnlmnomnopnopq");
  EXPECT_EQ(pieces.FinalHex(), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  Sha256 empty;
  EXPECT_EQ(empty.FinalHex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // Spent: never a valid hash again.
  EXPECT_EQ(empty.FinalHex(), "");
}

TEST(Sha256, Files) {
  testing::ScopedTempDir dir;
  const std::filesystem::path file = dir.path() / "f.bin";
  {
    std::ofstream out(file, std::ios::binary);
    for (int i = 0; i < 1000000; ++i) {
      out.put('a');
    }
  }
  std::optional<FileDigest> d = HashFile(file, 1000000);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->size, 1000000u);
  // FIPS 180-2: one million 'a'.
  EXPECT_EQ(d->sha256, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  EXPECT_FALSE(HashFile(file, 999999));  // Larger than allowed.
  EXPECT_FALSE(HashFile(dir.path() / "missing", 100));
}

}  // namespace
}  // namespace torfox::update
