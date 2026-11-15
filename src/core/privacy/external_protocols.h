// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_PRIVACY_EXTERNAL_PROTOCOLS_H_
#define TORFOX_CORE_PRIVACY_EXTERNAL_PROTOCOLS_H_

#include <cstddef>
#include <string_view>

// Links that open another app (mailto:, tel:, ms-settings:, ...).
namespace torfox::external_protocols {

inline constexpr std::size_t kMaxUrlLength = 2048;

// The scheme of |url| ("mailto" or "tel") if TorFox may offer to open it in
// the app the user chose for it; empty otherwise. Every other scheme is
// refused without asking.
//
// The URL also has to be safe to hand to the operating system: the mail or
// phone app gets it on its command line, so it must be printable ASCII with
// no spaces, quotes or other characters a command line could split on
// (Chromium has already percent-encoded anything else), and at most
// kMaxUrlLength bytes. The scheme must be lowercase, as Chromium writes it.
std::string_view LaunchableScheme(std::string_view url);

}  // namespace torfox::external_protocols

#endif  // TORFOX_CORE_PRIVACY_EXTERNAL_PROTOCOLS_H_
