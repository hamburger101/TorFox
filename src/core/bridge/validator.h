// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BRIDGE_VALIDATOR_H_
#define TORFOX_CORE_BRIDGE_VALIDATOR_H_

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "src/core/json.h"

namespace torfox::bridge {

// The bridge is the only channel from TorFox's internal pages to native code.
// Every message is checked here, in the browser process, before any handler
// sees it. The checks do not trust anything the renderer says about itself:
// |sender_is_internal| and |frame_url| come from the browser process's own
// records.

enum class ArgType { kString, kInt, kBool, kAny };

struct ArgSpec {
  std::string_view name;
  ArgType type;
  bool required;
  std::size_t max_length = 0;  // kString: maximum byte length.
  long long min = 0;           // kInt: inclusive range.
  long long max = 0;
};

struct CommandSpec {
  std::string_view name;
  std::span<const ArgSpec> args;
  bool persistent = false;  // Subscription-style query that stays open.
};

struct PageSpec {
  std::string_view host;  // torfox://<host>/
  std::span<const CommandSpec> commands;
};

struct Context {
  bool sender_is_internal = false;  // Browser is registered as internal.
  bool is_main_frame = false;
  std::string_view frame_url;       // The frame's committed URL.
  bool persistent = false;          // Query was sent with persistent=true.
};

enum class Reject {
  kNone,
  kNotInternalBrowser,
  kNotMainFrame,
  kBadOrigin,
  kUnknownPage,
  kTooLarge,
  kMalformed,
  kUnknownCommand,
  kPersistenceMismatch,
  kUnknownArgument,
  kMissingArgument,
  kBadArgumentType,
  kArgumentOutOfRange,
};

std::string_view RejectName(Reject reason);

struct Validated {
  Reject reject = Reject::kNone;
  std::string detail;  // Extra context for logs; never shown to web content.
  std::string page;
  std::string command;
  Json args;  // Always an object when reject == kNone.

  bool ok() const { return reject == Reject::kNone; }
};

inline constexpr std::size_t kMaxMessageBytes = 64 * 1024;
inline constexpr int kMaxMessageDepth = 6;

// Message format: {"cmd": "<name>", "args": {...}}. "args" may be omitted
// when the command takes no arguments. No other top-level members.
Validated Validate(std::span<const PageSpec> pages, const Context& ctx, std::string_view request);

// The pages and commands TorFox's UI is allowed to use.
std::span<const PageSpec> TorFoxPages();

}  // namespace torfox::bridge

#endif  // TORFOX_CORE_BRIDGE_VALIDATOR_H_
