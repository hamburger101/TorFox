// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_UPDATE_RELEASE_H_
#define TORFOX_CORE_UPDATE_RELEASE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/core/update/minisign.h"

namespace torfox::update {

// ----- Versions: MAJOR.MINOR.PATCH, numbers only ---------------------------------

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
};
// Each part 0..99999 without leading zeros; nothing else (no "v", no
// pre-release or build suffix).
std::optional<Version> ParseVersion(std::string_view text);
int CompareVersions(const Version& a, const Version& b);
std::string VersionString(const Version& v);

// ----- The signed release manifest ---------------------------------------------------
//
// manifest.json, signed with minisign (manifest.json.minisig) by the release
// workflow:
//   {"format": 1, "product": "TorFox", "version": "1.2.3",
//    "timestamp": <release time, Unix seconds>, "notes": "<release notes>",
//    "assets": {"windows-x64": {"name": "TorFox-1.2.3-win-x64-setup.exe",
//                               "size": <bytes>, "sha256": "<hex>"}, ...}}
// The signature's trusted comment is "torfox-release version:1.2.3
// timestamp:<same time>", and must agree with the manifest.

struct Asset {
  std::string name;
  uint64_t size = 0;
  std::string sha256;  // 64 lower-case hex digits.
};

struct Release {
  Version version;
  int64_t timestamp = 0;
  std::string notes;
  std::map<std::string, Asset> assets;  // By platform: "windows-x64", "macos-arm64", "macos-x64".
};

inline constexpr std::size_t kMaxManifestBytes = 64 * 1024;
inline constexpr std::size_t kMaxNotesBytes = 16 * 1024;
inline constexpr uint64_t kMaxAssetBytes = 1ull << 30;

struct ManifestCheck {
  bool ok = false;
  std::string error;
  Release release;
};

// The signature first; the manifest is parsed only once it verifies.
ManifestCheck CheckManifest(const PublicKey& key, std::string_view manifest,
                            std::string_view signature);

// "torfox-release version:<v> timestamp:<t>".
std::string TrustedComment(const Version& version, int64_t timestamp);

struct Offer {
  bool update = false;
  std::string reason;  // Why not, when |update| is false.
  Asset asset;
};

// Offer |release| only if it is strictly newer than |installed|, is not
// older than the newest release TorFox has already verified (|newest_seen|,
// its timestamp: an old signed manifest cannot be replayed to hold TorFox
// back), and has an asset for |platform|.
Offer Decide(const Release& release, const Version& installed, int64_t newest_seen,
             std::string_view platform);

// What TorFox remembers between runs (<user data>/update.json): when it last
// checked, and the timestamp of the newest release manifest it has verified.
struct SavedState {
  int64_t last_check = 0;
  int64_t newest_seen = 0;
};
inline constexpr std::size_t kMaxSavedStateBytes = 4096;
// A damaged or foreign file reads as nothing remembered.
SavedState ParseSavedState(std::string_view text);
std::string SerializeSavedState(const SavedState& state);

// ----- GitHub Releases ---------------------------------------------------------------

// Where TorFox looks for releases. Production: GitHub's API and download
// hosts, https only. Test builds may point every role at one local origin.
struct Endpoints {
  std::string api_origin = "https://api.github.com";
  std::string download_origin = "https://github.com";
  // Every URL fetched (API, assets, and each redirect) must be on one of
  // these origins.
  std::vector<std::string> allowed_origins = {
      "https://api.github.com", "https://github.com",
      "https://objects.githubusercontent.com", "https://release-assets.githubusercontent.com"};
};

// "owner/name" with GitHub's characters only.
bool IsValidRepository(std::string_view repository);
std::string LatestReleaseUrl(const Endpoints& endpoints, std::string_view repository);
// True if |url| is an absolute URL on one of |endpoints|' allowed origins,
// with no user name, password or unusual characters.
bool IsAllowedUrl(const Endpoints& endpoints, std::string_view url);

struct LatestRelease {
  std::string tag;
  std::string manifest_url;
  std::string signature_url;
  std::map<std::string, std::string> asset_urls;  // By file name.
};

inline constexpr std::size_t kMaxReleaseJsonBytes = 1 << 20;

// The "releases/latest" API answer. Download URLs must be
// <download_origin>/<repository>/releases/download/<tag>/<name>: assets of
// another repository are ignored.
std::optional<LatestRelease> ParseLatestRelease(const Endpoints& endpoints,
                                                std::string_view repository,
                                                std::string_view json, std::string* error);

}  // namespace torfox::update

#endif  // TORFOX_CORE_UPDATE_RELEASE_H_
