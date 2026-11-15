// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/update/release.h"

#include <algorithm>
#include <charconv>

#include "src/core/json.h"

namespace torfox::update {

namespace {

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsHex(std::string_view s) {
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return IsDigit(c) || (c >= 'a' && c <= 'f'); });
}

std::optional<int64_t> ParseInt(std::string_view s) {
  if (s.empty() || s.size() > 18 || !std::all_of(s.begin(), s.end(), IsDigit) ||
      (s.size() > 1 && s[0] == '0')) {
    return std::nullopt;
  }
  int64_t v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

// Release file names: "TorFox-<version>-<platform part>.<exe|dmg>", in
// letters, digits, '.', '-' and '_' only.
bool IsValidAssetName(std::string_view name, const Version& version) {
  const std::string prefix = "TorFox-" + VersionString(version) + "-";
  if (name.size() > 128 || name.substr(0, prefix.size()) != prefix) {
    return false;
  }
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '.' || c == '-' ||
          c == '_')) {
      return false;
    }
  }
  const auto ends = [&](std::string_view ext) {
    return name.size() > prefix.size() + ext.size() && name.substr(name.size() - ext.size()) == ext;
  };
  return (ends(".exe") || ends(".dmg")) && name.find("..") == std::string_view::npos;
}

ManifestCheck Fail(std::string why) {
  ManifestCheck c;
  c.error = std::move(why);
  return c;
}

// "scheme://host[:port]" of an absolute URL, or empty if the URL has
// anything TorFox does not expect in its authority.
std::string OriginOf(std::string_view url) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return {};
  }
  const std::string_view scheme = url.substr(0, scheme_end);
  if (scheme != "https" && scheme != "http") {
    return {};
  }
  const std::size_t host_start = scheme_end + 3;
  std::size_t host_end = url.find_first_of("/?#", host_start);
  if (host_end == std::string_view::npos) {
    host_end = url.size();
  }
  const std::string_view authority = url.substr(host_start, host_end - host_start);
  if (authority.empty()) {
    return {};
  }
  for (char c : authority) {
    // No user info ('@'), no escapes, no upper case: an exact host only.
    if (!((c >= 'a' && c <= 'z') || IsDigit(c) || c == '.' || c == '-' || c == ':')) {
      return {};
    }
  }
  return std::string(url.substr(0, host_end));
}

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
  Version v;
  int* parts[3] = {&v.major, &v.minor, &v.patch};
  for (int i = 0; i < 3; ++i) {
    const std::size_t dot = text.find('.');
    const std::string_view part = i < 2 ? text.substr(0, dot) : text;
    if (i < 2 && dot == std::string_view::npos) {
      return std::nullopt;
    }
    std::optional<int64_t> n = ParseInt(part);
    if (!n || *n > 99999) {
      return std::nullopt;
    }
    *parts[i] = static_cast<int>(*n);
    if (i < 2) {
      text.remove_prefix(dot + 1);
    }
  }
  return v;
}

int CompareVersions(const Version& a, const Version& b) {
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  return 0;
}

std::string VersionString(const Version& v) {
  return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

std::string TrustedComment(const Version& version, int64_t timestamp) {
  return "torfox-release version:" + VersionString(version) +
         " timestamp:" + std::to_string(timestamp);
}

ManifestCheck CheckManifest(const PublicKey& key, std::string_view manifest,
                            std::string_view signature) {
  if (manifest.size() > kMaxManifestBytes) {
    return Fail("the release manifest is too large");
  }
  const SignatureCheck sig = VerifyMinisign(key, signature, manifest);
  if (!sig.ok) {
    return Fail("the release manifest's signature is not valid: " + sig.error);
  }
  std::optional<Json> j = ParseJson(manifest, kMaxManifestBytes, 6);
  if (!j || !j->is_object()) {
    return Fail("the release manifest is not valid JSON");
  }
  const std::optional<long long> format = GetInt(*j, "format");
  if (!format || *format != 1 || GetString(*j, "product").value_or("") != "TorFox") {
    return Fail("the release manifest is not a TorFox manifest TorFox understands");
  }
  ManifestCheck c;
  std::optional<Version> version = ParseVersion(GetString(*j, "version").value_or(""));
  const std::optional<long long> timestamp = GetInt(*j, "timestamp");
  if (!version || !timestamp || *timestamp <= 0) {
    return Fail("the release manifest has no valid version or time");
  }
  if (sig.trusted_comment != TrustedComment(*version, *timestamp)) {
    return Fail("the signed comment does not match the release manifest");
  }
  c.release.version = *version;
  c.release.timestamp = *timestamp;
  c.release.notes = GetString(*j, "notes").value_or("");
  if (c.release.notes.size() > kMaxNotesBytes) {
    return Fail("the release notes are too long");
  }
  const Json* assets = GetMember(*j, "assets");
  if (!assets || !assets->is_object() || assets->size() > 8) {
    return Fail("the release manifest lists no assets");
  }
  for (auto it = assets->begin(); it != assets->end(); ++it) {
    const std::string& platform = it.key();
    if (platform != "windows-x64" && platform != "macos-arm64" && platform != "macos-x64") {
      continue;  // A platform this version of TorFox does not know.
    }
    Asset a;
    a.name = GetString(*it, "name").value_or("");
    a.sha256 = GetString(*it, "sha256").value_or("");
    const std::optional<long long> size = GetInt(*it, "size");
    if (!IsValidAssetName(a.name, *version) || a.sha256.size() != 64 || !IsHex(a.sha256) || !size ||
        *size <= 0 || static_cast<uint64_t>(*size) > kMaxAssetBytes) {
      return Fail("the release manifest has an invalid entry for " + platform);
    }
    a.size = static_cast<uint64_t>(*size);
    c.release.assets[platform] = std::move(a);
  }
  c.ok = true;
  return c;
}

Offer Decide(const Release& release, const Version& installed, int64_t newest_seen,
             std::string_view platform) {
  Offer o;
  if (CompareVersions(release.version, installed) <= 0) {
    o.reason = "TorFox " + VersionString(installed) + " is up to date";
    return o;
  }
  if (release.timestamp < newest_seen) {
    o.reason = "this release is older than one TorFox has already seen";
    return o;
  }
  auto it = release.assets.find(std::string(platform));
  if (it == release.assets.end()) {
    o.reason = "this release has no file for this computer";
    return o;
  }
  o.update = true;
  o.asset = it->second;
  return o;
}

SavedState ParseSavedState(std::string_view text) {
  SavedState s;
  std::optional<Json> j = ParseJson(text, kMaxSavedStateBytes, 2);
  if (!j || !j->is_object() || GetInt(*j, "format").value_or(0) != 1) {
    return s;
  }
  s.last_check = std::max<long long>(0, GetInt(*j, "lastCheck").value_or(0));
  s.newest_seen = std::max<long long>(0, GetInt(*j, "newestSeen").value_or(0));
  return s;
}

std::string SerializeSavedState(const SavedState& state) {
  return DumpJson({{"format", 1}, {"lastCheck", state.last_check}, {"newestSeen", state.newest_seen}},
                  true);
}

bool IsValidRepository(std::string_view repository) {
  const std::size_t slash = repository.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 == repository.size() ||
      repository.size() > 140) {
    return false;
  }
  for (std::size_t i = 0; i < repository.size(); ++i) {
    const char c = repository[i];
    if (i == slash) {
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '-' || c == '_' ||
          c == '.')) {
      return false;
    }
  }
  return repository.find("..") == std::string_view::npos &&
         repository.find('/', slash + 1) == std::string_view::npos;
}

std::string LatestReleaseUrl(const Endpoints& endpoints, std::string_view repository) {
  return endpoints.api_origin + "/repos/" + std::string(repository) + "/releases/latest";
}

bool IsAllowedUrl(const Endpoints& endpoints, std::string_view url) {
  if (url.size() > 8192) {
    return false;
  }
  for (char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F || c == '\\' ||
        static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
  }
  const std::string origin = OriginOf(url);
  if (origin.empty()) {
    return false;
  }
  return std::find(endpoints.allowed_origins.begin(), endpoints.allowed_origins.end(), origin) !=
         endpoints.allowed_origins.end();
}

std::optional<LatestRelease> ParseLatestRelease(const Endpoints& endpoints,
                                                std::string_view repository,
                                                std::string_view json, std::string* error) {
  auto fail = [error](const char* why) -> std::optional<LatestRelease> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };
  std::optional<Json> j = ParseJson(json, kMaxReleaseJsonBytes, 16);
  if (!j || !j->is_object()) {
    return fail("GitHub's answer is not valid JSON");
  }
  LatestRelease r;
  r.tag = GetString(*j, "tag_name").value_or("");
  if (r.tag.empty() || r.tag.size() > 64 ||
      !std::all_of(r.tag.begin(), r.tag.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '.' ||
               c == '-' || c == '_';
      })) {
    return fail("the latest release has no valid tag");
  }
  if (GetBool(*j, "draft").value_or(false) || GetBool(*j, "prerelease").value_or(false)) {
    return fail("the latest release is a draft or a pre-release");
  }
  const Json* assets = GetMember(*j, "assets");
  if (!assets || !assets->is_array()) {
    return fail("the latest release lists no files");
  }
  const std::string base = endpoints.download_origin + "/" + std::string(repository) +
                           "/releases/download/" + r.tag + "/";
  for (const Json& a : *assets) {
    const std::string name = GetString(a, "name").value_or("");
    const std::string url = GetString(a, "browser_download_url").value_or("");
    if (name.empty() || url != base + name || !IsAllowedUrl(endpoints, url)) {
      continue;
    }
    if (name == "manifest.json") {
      r.manifest_url = url;
    } else if (name == "manifest.json.minisig") {
      r.signature_url = url;
    } else {
      r.asset_urls[name] = url;
    }
  }
  if (r.manifest_url.empty() || r.signature_url.empty()) {
    return fail("the latest release has no signed manifest");
  }
  return r;
}

}  // namespace torfox::update
