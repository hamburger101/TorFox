// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_DOWNLOADS_POLICY_H_
#define TORFOX_CORE_DOWNLOADS_POLICY_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace torfox::downloads {

// The two download settings (Settings > Downloads).
inline constexpr std::string_view kAskSetting = "downloads.askBeforeDownload";
inline constexpr std::string_view kDisableSetting = "downloads.disableDownloads";

enum class Decision { kBlock, kAsk, kAllow };

// The one download decision. The browser applies it before any bytes are
// fetched (CefDownloadHandler::CanDownload) and again before a file is
// created (OnBeforeDownload), and cancels running downloads when it becomes
// kBlock. "Disable downloads" overrides "Ask before download".
Decision Decide(bool disable_downloads, bool ask_before_download);

// A file name that is safe to create from a server- or page-suggested name
// (UTF-8): only the last path component; control, bidirectional-override and
// zero-width characters and characters Windows or macOS reserve are removed or
// replaced; leading dots and trailing dots or spaces are trimmed; reserved
// Windows device names are prefixed; at most kMaxNameBytes bytes, keeping the
// extension. Never empty ("download").
inline constexpr std::size_t kMaxNameBytes = 200;
std::string SafeFileName(std::string_view suggested);

// What TorFox appends to a download's name while it is unfinished.
inline constexpr std::string_view kPartialSuffix = ".crdownload";

// The longest path TorFox gives a download, in native units (UTF-16 code
// units on Windows, bytes elsewhere). On Windows this is MAX_PATH less its
// terminator: Chromium's download code fails on longer paths, and then
// cannot delete what it wrote (docs/STAGE3_FINDINGS.md).
#if defined(_WIN32)
inline constexpr std::size_t kMaxPathLength = 259;
#else
inline constexpr std::size_t kMaxPathLength = 1023;
#endif

// |dir|/|name|, or "<stem> (n)<ext>" for the first n in 1..9999 that doesn't
// exist, so an existing file is never overwritten. The stem is shortened
// (whole characters, keeping the extension) until the path, plus |reserve|
// more units, fits in kMaxPathLength. nullopt if every name is taken, or if
// |dir| leaves no room for a name.
std::optional<std::filesystem::path> UniquePath(
    const std::filesystem::path& dir,
    std::string_view name,
    const std::function<bool(const std::filesystem::path&)>& exists,
    std::size_t reserve = 0);

// Types that run code when opened (programs, scripts, shortcuts, installers,
// disk images). The confirmation dialog warns about them.
bool IsDangerousType(std::string_view name);

// "512 bytes", "1.5 KB", "12.0 MB", "1.2 GB"; "Unknown size" when negative.
std::string FormatSize(int64_t bytes);

// "https://example.com" for http(s) and blob: URLs of http(s) origins,
// "" otherwise (data: URLs and other schemes have no origin to show).
std::string DisplayOrigin(std::string_view url);

}  // namespace torfox::downloads

#endif  // TORFOX_CORE_DOWNLOADS_POLICY_H_
