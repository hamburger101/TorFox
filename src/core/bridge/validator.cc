// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/bridge/validator.h"

#include <array>
#include <climits>

#include "src/core/url/internal_url.h"
#include "src/core/url/nav_input.h"

namespace torfox::bridge {

namespace {

// ----- Command tables -------------------------------------------------------

constexpr std::array<ArgSpec, 0> kNoArgs = {};
constexpr std::array<ArgSpec, 1> kTabIdArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, INT_MAX},
};
constexpr std::array<ArgSpec, 1> kNavGoArgs = {
    ArgSpec{"input", ArgType::kString, true, kMaxNavInputLength, 0, 0},
};
constexpr std::array<ArgSpec, 2> kSettingsSetArgs = {
    ArgSpec{"key", ArgType::kString, true, 128, 0, 0},
    ArgSpec{"value", ArgType::kAny, true, 0, 0, 0},
};

constexpr CommandSpec Cmd(std::string_view name, std::span<const ArgSpec> args,
                          bool persistent = false) {
  return CommandSpec{name, args, persistent};
}

const std::array<CommandSpec, 11> kShellCommands = {
    Cmd("state.subscribe", kNoArgs, true),
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("tab.new", kNoArgs),
    Cmd("tab.close", kTabIdArgs),
    Cmd("tab.select", kTabIdArgs),
    Cmd("nav.go", kNavGoArgs),
    Cmd("nav.back", kNoArgs),
    Cmd("nav.forward", kNoArgs),
    Cmd("nav.reload", kNoArgs),
    Cmd("nav.stop", kNoArgs),
    Cmd("app.openSettings", kNoArgs),
};

const std::array<CommandSpec, 2> kNewTabCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("nav.go", kNavGoArgs),
};

const std::array<CommandSpec, 4> kSettingsCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("settings.get", kNoArgs),
    Cmd("settings.set", kSettingsSetArgs),
    Cmd("app.info", kNoArgs),
};

const std::array<PageSpec, 3> kPages = {
    PageSpec{"shell", kShellCommands},
    PageSpec{"newtab", kNewTabCommands},
    PageSpec{"settings", kSettingsCommands},
};

// ----- Validation -----------------------------------------------------------

Validated Fail(Reject reason, std::string detail = {}) {
  Validated v;
  v.reject = reason;
  v.detail = std::move(detail);
  return v;
}

bool ValidUtf8NoControls(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
    std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                                 : (c & 0xF8) == 0xF0     ? 4
                                                          : 0;
    if (len == 0 || i + len > s.size()) {
      return false;
    }
    for (std::size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        return false;
      }
    }
    i += len;
  }
  return true;
}

}  // namespace

std::string_view RejectName(Reject reason) {
  switch (reason) {
    case Reject::kNone:
      return "none";
    case Reject::kNotInternalBrowser:
      return "not_internal_browser";
    case Reject::kNotMainFrame:
      return "not_main_frame";
    case Reject::kBadOrigin:
      return "bad_origin";
    case Reject::kUnknownPage:
      return "unknown_page";
    case Reject::kTooLarge:
      return "too_large";
    case Reject::kMalformed:
      return "malformed";
    case Reject::kUnknownCommand:
      return "unknown_command";
    case Reject::kPersistenceMismatch:
      return "persistence_mismatch";
    case Reject::kUnknownArgument:
      return "unknown_argument";
    case Reject::kMissingArgument:
      return "missing_argument";
    case Reject::kBadArgumentType:
      return "bad_argument_type";
    case Reject::kArgumentOutOfRange:
      return "argument_out_of_range";
  }
  return "unknown";
}

std::span<const PageSpec> TorFoxPages() {
  return std::span<const PageSpec>(kPages);
}

Validated Validate(std::span<const PageSpec> pages, const Context& ctx, std::string_view request) {
  // 1. Who is sending: only registered internal browsers, only main frames.
  if (!ctx.sender_is_internal) {
    return Fail(Reject::kNotInternalBrowser);
  }
  if (!ctx.is_main_frame) {
    return Fail(Reject::kNotMainFrame);
  }

  // 2. From which page: the committed origin must be a known internal page.
  auto url = ParseInternalUrl(ctx.frame_url);
  if (!url) {
    return Fail(Reject::kBadOrigin, std::string(ctx.frame_url.substr(0, 200)));
  }
  const PageSpec* page = nullptr;
  for (const PageSpec& p : pages) {
    if (p.host == url->host) {
      page = &p;
      break;
    }
  }
  if (!page) {
    return Fail(Reject::kUnknownPage, url->host);
  }

  // 3. What is being asked: a well-formed message within size limits.
  if (request.size() > kMaxMessageBytes) {
    return Fail(Reject::kTooLarge);
  }
  auto parsed = ParseJson(request, kMaxMessageBytes, kMaxMessageDepth);
  if (!parsed || !parsed->is_object()) {
    return Fail(Reject::kMalformed, "not a JSON object");
  }
  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
    if (it.key() != "cmd" && it.key() != "args") {
      return Fail(Reject::kMalformed, "unexpected member");
    }
  }
  auto cmd = GetString(*parsed, "cmd");
  if (!cmd || cmd->empty() || cmd->size() > 64) {
    return Fail(Reject::kMalformed, "missing cmd");
  }
  const Json* args = GetMember(*parsed, "args");
  if (args && !args->is_object()) {
    return Fail(Reject::kMalformed, "args is not an object");
  }

  // 4. Is this page allowed to ask for it, in this form?
  const CommandSpec* spec = nullptr;
  for (const CommandSpec& c : page->commands) {
    if (c.name == *cmd) {
      spec = &c;
      break;
    }
  }
  if (!spec) {
    return Fail(Reject::kUnknownCommand, *cmd);
  }
  if (spec->persistent != ctx.persistent) {
    return Fail(Reject::kPersistenceMismatch, *cmd);
  }

  // 5. Arguments: no unknown names, required ones present, types and ranges.
  Json empty = Json::object();
  const Json& a = args ? *args : empty;
  for (auto it = a.begin(); it != a.end(); ++it) {
    bool known = false;
    for (const ArgSpec& s : spec->args) {
      if (s.name == it.key()) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Fail(Reject::kUnknownArgument, it.key());
    }
  }
  for (const ArgSpec& s : spec->args) {
    const Json* v = GetMember(a, s.name);
    if (!v) {
      if (s.required) {
        return Fail(Reject::kMissingArgument, std::string(s.name));
      }
      continue;
    }
    switch (s.type) {
      case ArgType::kString: {
        if (!v->is_string()) {
          return Fail(Reject::kBadArgumentType, std::string(s.name));
        }
        const std::string& str = v->get_ref<const std::string&>();
        if (str.size() > s.max_length) {
          return Fail(Reject::kArgumentOutOfRange, std::string(s.name));
        }
        if (!ValidUtf8NoControls(str)) {
          return Fail(Reject::kBadArgumentType, std::string(s.name));
        }
        break;
      }
      case ArgType::kInt: {
        if (!v->is_number_integer()) {
          return Fail(Reject::kBadArgumentType, std::string(s.name));
        }
        if (v->is_number_unsigned() && v->get<unsigned long long>() > LLONG_MAX) {
          return Fail(Reject::kArgumentOutOfRange, std::string(s.name));
        }
        long long n = v->get<long long>();
        if (n < s.min || n > s.max) {
          return Fail(Reject::kArgumentOutOfRange, std::string(s.name));
        }
        break;
      }
      case ArgType::kBool:
        if (!v->is_boolean()) {
          return Fail(Reject::kBadArgumentType, std::string(s.name));
        }
        break;
      case ArgType::kAny:
        // Validated by the handler against its own schema (e.g. settings).
        break;
    }
  }

  Validated ok;
  ok.page = url->host;
  ok.command = *cmd;
  ok.args = a;
  return ok;
}

}  // namespace torfox::bridge
