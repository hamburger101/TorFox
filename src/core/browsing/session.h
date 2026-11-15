// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_SESSION_H_
#define TORFOX_CORE_BROWSING_SESSION_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace torfox::browsing {

// The tabs of TorFox's normal windows, kept for Settings > General >
// On startup > "Restore previous tabs". Private and Tor windows are never
// part of a session, and neither are onion addresses.
struct SessionWindow {
  std::vector<std::string> urls;  // In tab order.
  std::size_t active = 0;         // Index into |urls|.
};
using Session = std::vector<SessionWindow>;

inline constexpr std::string_view kStartupSetting = "general.startupPage";
inline constexpr std::size_t kMaxSessionWindows = 32;
inline constexpr std::size_t kMaxSessionTabs = 256;  // Per window.
inline constexpr std::size_t kMaxSessionBytes = 8u << 20;  // A larger file is ignored.

// What a session keeps of a tab's URL: a web URL Places would store (and no
// onion host), or TorFox's own page "torfox://<page>/" (not the shell; any
// path or query dropped). Anything else is not kept.
std::optional<std::string> RestorableUrl(std::string_view url);

// {"format":1,"windows":[{"tabs":["https://...", ...],"active":0}, ...]}.
// Tabs whose URL is not restorable are left out, and so are windows left
// with no tab.
std::string SerializeSession(const Session& session);

// Never fails: anything unreadable, unknown or over a limit is dropped, and
// |active| is clamped to the window's tabs. An empty result means nothing
// to restore.
Session ParseSession(std::string_view text);

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_SESSION_H_
