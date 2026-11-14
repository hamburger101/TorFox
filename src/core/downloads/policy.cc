// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/downloads/policy.h"

#include <array>
#include <cstdio>
#include <vector>

#include "src/core/util/utf8.h"

namespace torfox::downloads {

namespace {

constexpr char32_t kReplacement = U'_';

bool IsDroppedCodePoint(char32_t c) {
  // Bidirectional controls (can disguise "exe.txt" as "txt.exe") and
  // zero-width characters.
  return c == 0x061C || c == 0x200B || c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F ||
         (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

bool IsReplacedCodePoint(char32_t c) {
  if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F)) {
    return true;  // C0 and C1 controls.
  }
  switch (c) {
    case U'<':
    case U'>':
    case U':':
    case U'"':
    case U'|':
    case U'?':
    case U'*':
    case U'/':
    case U'\\':
      return true;
    default:
      return false;
  }
}

bool IsTrimmed(char32_t c) {
  return c == U' ' || c == U'.' || c == 0x00A0 || c == 0x3000;
}

char32_t AsciiUpper(char32_t c) {
  return c >= U'a' && c <= U'z' ? c - 32 : c;
}

// Windows device names, reserved whatever the extension ("con.txt").
bool IsReservedStem(const std::u32string& stem) {
  std::u32string upper;
  for (char32_t c : stem) {
    upper.push_back(AsciiUpper(c));
  }
  // Windows also ignores trailing spaces before the extension ("CON .txt").
  while (!upper.empty() && upper.back() == U' ') {
    upper.pop_back();
  }
  static const std::array<std::u32string_view, 6> kNames = {U"CON", U"PRN", U"AUX", U"NUL",
                                                            U"CONIN$", U"CONOUT$"};
  for (std::u32string_view name : kNames) {
    if (upper == name) {
      return true;
    }
  }
  if (upper.size() == 4 && (upper.compare(0, 3, U"COM") == 0 || upper.compare(0, 3, U"LPT") == 0)) {
    const char32_t d = upper[3];
    return (d >= U'0' && d <= U'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3;
  }
  return false;
}

std::size_t LastDot(const std::u32string& name) {
  const std::size_t dot = name.rfind(U'.');
  return dot == std::u32string::npos || dot == 0 ? name.size() : dot;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

}  // namespace

Decision Decide(bool disable_downloads, bool ask_before_download) {
  if (disable_downloads) {
    return Decision::kBlock;
  }
  return ask_before_download ? Decision::kAsk : Decision::kAllow;
}

std::string SafeFileName(std::string_view suggested) {
  std::u32string decoded = utf8::Decode(suggested, kReplacement);
  // Only the last path component.
  const std::size_t sep = decoded.find_last_of(U"/\\");
  if (sep != std::u32string::npos) {
    decoded.erase(0, sep + 1);
  }
  std::u32string name;
  for (char32_t c : decoded) {
    if (IsDroppedCodePoint(c)) {
      continue;
    }
    name.push_back(IsReplacedCodePoint(c) ? kReplacement : c);
  }
  std::size_t begin = 0;
  while (begin < name.size() && IsTrimmed(name[begin])) {
    ++begin;
  }
  std::size_t end = name.size();
  while (end > begin && IsTrimmed(name[end - 1])) {
    --end;
  }
  name = name.substr(begin, end - begin);
  if (name.empty()) {
    return "download";
  }
  const std::size_t first_dot = name.find(U'.');
  if (IsReservedStem(name.substr(0, first_dot))) {
    name.insert(name.begin(), U'_');
  }

  // Byte limit, keeping a short extension.
  std::string out = utf8::Encode(name);
  if (out.size() <= kMaxNameBytes) {
    return out;
  }
  const std::size_t dot = LastDot(name);
  std::u32string ext = name.substr(dot);
  if (utf8::Encode(ext).size() > 20) {
    ext.clear();
  }
  std::u32string stem = name.substr(0, dot == name.size() || ext.empty() ? name.size() : dot);
  const std::size_t ext_bytes = utf8::Encode(ext).size();
  std::string stem_bytes;
  for (char32_t c : stem) {
    std::string next = utf8::Encode(std::u32string(1, c));
    if (stem_bytes.size() + next.size() + ext_bytes > kMaxNameBytes) {
      break;
    }
    stem_bytes += next;
  }
  // Cutting may leave trailing spaces or dots before the extension.
  while (!stem_bytes.empty() && (stem_bytes.back() == ' ' || stem_bytes.back() == '.')) {
    stem_bytes.pop_back();
  }
  if (stem_bytes.empty()) {
    stem_bytes = "download";
  }
  return stem_bytes + utf8::Encode(ext);
}

namespace {

std::filesystem::path Utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// dir/(stem + tail), with |stem| shortened until the path and |reserve| more
// units fit in kMaxPathLength; nullopt if no stem fits.
std::optional<std::filesystem::path> Fit(const std::filesystem::path& dir,
                                         std::u32string stem,
                                         const std::string& tail,
                                         std::size_t reserve) {
  while (!stem.empty()) {
    std::filesystem::path path = dir / Utf8Path(utf8::Encode(stem) + tail);
    if (path.native().size() + reserve <= kMaxPathLength) {
      return path;
    }
    stem.pop_back();
    // A cut may leave trailing spaces or dots, which Windows drops.
    while (!stem.empty() && IsTrimmed(stem.back())) {
      stem.pop_back();
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::filesystem::path> UniquePath(
    const std::filesystem::path& dir,
    std::string_view name,
    const std::function<bool(const std::filesystem::path&)>& exists,
    std::size_t reserve) {
  const std::u32string wide = utf8::Decode(name, kReplacement);
  const std::size_t dot = LastDot(wide);
  const std::u32string stem = wide.substr(0, dot);
  const std::string ext = utf8::Encode(wide.substr(dot));
  for (int n = 0; n < 10000; ++n) {
    const std::string tail = n == 0 ? ext : " (" + std::to_string(n) + ")" + ext;
    std::optional<std::filesystem::path> path = Fit(dir, stem, tail, reserve);
    if (!path) {
      return std::nullopt;  // No room in |dir| for any name.
    }
    if (!exists(*path)) {
      return path;
    }
  }
  return std::nullopt;
}

bool IsDangerousType(std::string_view name) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  const std::string ext = Lower(name.substr(dot + 1));
  static constexpr std::string_view kTypes[] = {
      // Windows programs, installers, scripts and shortcuts.
      "exe", "com", "scr", "pif", "cpl", "msi", "msp", "mst", "msix", "msixbundle", "appx",
      "appxbundle", "appinstaller", "application", "appref-ms", "bat", "cmd", "ps1", "psm1", "psd1",
      "vbs", "vbe", "js", "jse", "wsf", "wsh", "wsc", "hta", "lnk", "url", "scf", "reg", "inf",
      "dll", "sys", "chm", "gadget", "settingcontent-ms", "library-ms", "jar", "iso", "img", "vhd",
      "vhdx",
      // macOS and Unix.
      "app", "pkg", "mpkg", "dmg", "command", "tool", "sh", "zsh", "terminal", "webloc",
      "workflow", "action", "scpt", "applescript"};
  for (std::string_view type : kTypes) {
    if (ext == type) {
      return true;
    }
  }
  return false;
}

std::string FormatSize(int64_t bytes) {
  if (bytes < 0) {
    return "Unknown size";
  }
  if (bytes < 1024) {
    return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
  }
  static const std::array<const char*, 4> kUnits = {"KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes) / 1024.0;
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  return buf;
}

std::string DisplayOrigin(std::string_view url) {
  if (url.substr(0, 5) == "blob:") {
    url.remove_prefix(5);
  }
  std::size_t start = 0;
  if (url.substr(0, 8) == "https://") {
    start = 8;
  } else if (url.substr(0, 7) == "http://") {
    start = 7;
  } else {
    return std::string();
  }
  const std::size_t end = url.find_first_of("/?#", start);
  std::string_view authority = url.substr(start, end == std::string_view::npos ? url.npos : end - start);
  // Never show credentials ("user:pass@host").
  const std::size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority.remove_prefix(at + 1);
  }
  if (authority.empty()) {
    return std::string();
  }
  return Lower(url.substr(0, start)) + Lower(authority);
}

}  // namespace torfox::downloads
