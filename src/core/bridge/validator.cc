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

constexpr long long kMaxJsInt = (1LL << 53) - 1;  // Largest exact JavaScript integer.
constexpr long long kMaxScreen = 100000;          // DIP coordinates in a page.

constexpr std::array<ArgSpec, 0> kNoArgs = {};
constexpr std::array<ArgSpec, 1> kTabIdArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, INT_MAX},
};
constexpr std::array<ArgSpec, 2> kTabMoveArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, INT_MAX},
    ArgSpec{"index", ArgType::kInt, true, 0, 0, 10000},
};
constexpr std::array<ArgSpec, 1> kNavGoArgs = {
    ArgSpec{"input", ArgType::kString, true, kMaxNavInputLength, 0, 0},
};
// Opens a stored http(s) URL (bookmark or history entry); the browser
// classifies it again exactly like typed input.
constexpr std::array<ArgSpec, 2> kNavOpenArgs = {
    ArgSpec{"url", ArgType::kString, true, kMaxNavInputLength, 0, 0},
    ArgSpec{"newTab", ArgType::kBool, false, 0, 0, 0},
};
constexpr std::array<ArgSpec, 3> kFindArgs = {
    ArgSpec{"text", ArgType::kString, true, 1024, 0, 0},
    ArgSpec{"forward", ArgType::kBool, false, 0, 0, 0},
    ArgSpec{"matchCase", ArgType::kBool, false, 0, 0, 0},
};
constexpr std::array<ArgSpec, 1> kZoomArgs = {
    ArgSpec{"step", ArgType::kInt, true, 0, -1, 1},  // -1 out, 0 reset, 1 in.
};
constexpr std::array<ArgSpec, 2> kMenuArgs = {
    ArgSpec{"x", ArgType::kInt, true, 0, 0, kMaxScreen},
    ArgSpec{"y", ArgType::kInt, true, 0, 0, kMaxScreen},
};
constexpr std::array<ArgSpec, 3> kFolderMenuArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"x", ArgType::kInt, true, 0, 0, kMaxScreen},
    ArgSpec{"y", ArgType::kInt, true, 0, 0, kMaxScreen},
};
constexpr std::array<ArgSpec, 2> kSettingsSetArgs = {
    ArgSpec{"key", ArgType::kString, true, 128, 0, 0},
    ArgSpec{"value", ArgType::kAny, true, 0, 0, 0},
};

// Bookmarks. Titles longer than the store keeps are cut by the store; the
// limit here only bounds the message.
constexpr std::size_t kMaxTitleArg = 4096;
constexpr std::array<ArgSpec, 1> kBookmarkIdArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, kMaxJsInt},
};
constexpr std::array<ArgSpec, 1> kBookmarkSearchArgs = {
    ArgSpec{"query", ArgType::kString, true, 256, 0, 0},
};
constexpr std::array<ArgSpec, 4> kBookmarkAddArgs = {
    ArgSpec{"parent", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"title", ArgType::kString, true, kMaxTitleArg, 0, 0},
    ArgSpec{"url", ArgType::kString, true, kMaxNavInputLength, 0, 0},
    ArgSpec{"index", ArgType::kInt, false, 0, -1, 100000},
};
constexpr std::array<ArgSpec, 3> kFolderAddArgs = {
    ArgSpec{"parent", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"title", ArgType::kString, true, kMaxTitleArg, 0, 0},
    ArgSpec{"index", ArgType::kInt, false, 0, -1, 100000},
};
constexpr std::array<ArgSpec, 3> kBookmarkUpdateArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"title", ArgType::kString, true, kMaxTitleArg, 0, 0},
    ArgSpec{"url", ArgType::kString, false, kMaxNavInputLength, 0, 0},
};
constexpr std::array<ArgSpec, 3> kBookmarkMoveArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"parent", ArgType::kInt, true, 0, 1, kMaxJsInt},
    ArgSpec{"index", ArgType::kInt, true, 0, -1, 100000},
};

// History.
constexpr std::array<ArgSpec, 3> kHistoryListArgs = {
    ArgSpec{"query", ArgType::kString, false, 256, 0, 0},
    ArgSpec{"before", ArgType::kInt, false, 0, 0, kMaxJsInt},
    ArgSpec{"limit", ArgType::kInt, false, 0, 1, 500},
};
constexpr std::array<ArgSpec, 1> kHistoryRemoveArgs = {
    ArgSpec{"url", ArgType::kString, true, kMaxNavInputLength, 0, 0},
};

// Address-bar suggestions. The shell sends what is typed and where the
// address bar is; the list page only says which row was clicked.
constexpr std::array<ArgSpec, 4> kSuggestQueryArgs = {
    ArgSpec{"text", ArgType::kString, true, 256, 0, 0},
    ArgSpec{"x", ArgType::kInt, true, 0, 0, kMaxScreen},
    ArgSpec{"y", ArgType::kInt, true, 0, 0, kMaxScreen},
    ArgSpec{"width", ArgType::kInt, true, 0, 1, kMaxScreen},
};
constexpr std::array<ArgSpec, 1> kSuggestHighlightArgs = {
    ArgSpec{"index", ArgType::kInt, true, 0, -1, kMaxSuggestions - 1},
};
constexpr std::array<ArgSpec, 1> kSuggestOpenArgs = {
    ArgSpec{"index", ArgType::kInt, true, 0, 0, kMaxSuggestions - 1},
};

// Downloads: Chromium's download ids are uint32.
constexpr std::array<ArgSpec, 1> kDownloadIdArgs = {
    ArgSpec{"id", ArgType::kInt, true, 0, 1, 0xFFFFFFFFLL},
};

constexpr CommandSpec Cmd(std::string_view name, std::span<const ArgSpec> args,
                          bool persistent = false) {
  return CommandSpec{name, args, persistent};
}

const std::array<CommandSpec, 33> kShellCommands = {
    Cmd("state.subscribe", kNoArgs, true),
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("tab.new", kNoArgs),
    Cmd("tab.close", kTabIdArgs),
    Cmd("tab.select", kTabIdArgs),
    Cmd("tab.move", kTabMoveArgs),
    Cmd("tab.reopen", kNoArgs),
    Cmd("window.new", kNoArgs),
    Cmd("window.newPrivate", kNoArgs),
    Cmd("window.newTor", kNoArgs),
    Cmd("tor.newIdentity", kNoArgs),
    Cmd("tor.connect", kNoArgs),
    Cmd("nav.go", kNavGoArgs),
    Cmd("nav.open", kNavOpenArgs),
    Cmd("nav.back", kNoArgs),
    Cmd("nav.forward", kNoArgs),
    Cmd("nav.reload", kNoArgs),
    Cmd("nav.stop", kNoArgs),
    Cmd("nav.home", kNoArgs),
    Cmd("find.query", kFindArgs),
    Cmd("find.stop", kNoArgs),
    Cmd("zoom.change", kZoomArgs),
    Cmd("bookmarks.star", kMenuArgs),
    Cmd("bookmarks.folderMenu", kFolderMenuArgs),
    Cmd("app.menu", kMenuArgs),
    Cmd("app.openSettings", kNoArgs),
    Cmd("app.openBookmarks", kNoArgs),
    Cmd("app.openHistory", kNoArgs),
    Cmd("app.openDownloads", kNoArgs),
    Cmd("app.focusContent", kNoArgs),
    Cmd("suggest.query", kSuggestQueryArgs),
    Cmd("suggest.highlight", kSuggestHighlightArgs),
    Cmd("suggest.hide", kNoArgs),
};

const std::array<CommandSpec, 3> kSuggestCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("suggest.subscribe", kNoArgs, true),
    Cmd("suggest.open", kSuggestOpenArgs),
};

const std::array<CommandSpec, 2> kNewTabCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("nav.go", kNavGoArgs),
};

const std::array<CommandSpec, 9> kSettingsCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("settings.get", kNoArgs),
    Cmd("settings.set", kSettingsSetArgs),
    Cmd("app.info", kNoArgs),
    // Settings > Updates. TorFox decides what each one may do in its state.
    Cmd("update.status", kNoArgs),
    Cmd("update.subscribe", kNoArgs, true),
    Cmd("update.check", kNoArgs),
    Cmd("update.download", kNoArgs),
    Cmd("update.install", kNoArgs),
};

const std::array<CommandSpec, 11> kBookmarksCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("bookmarks.subscribe", kNoArgs, true),
    Cmd("bookmarks.children", kBookmarkIdArgs),
    Cmd("bookmarks.folders", kNoArgs),
    Cmd("bookmarks.search", kBookmarkSearchArgs),
    Cmd("bookmarks.add", kBookmarkAddArgs),
    Cmd("bookmarks.addFolder", kFolderAddArgs),
    Cmd("bookmarks.update", kBookmarkUpdateArgs),
    Cmd("bookmarks.move", kBookmarkMoveArgs),
    Cmd("bookmarks.remove", kBookmarkIdArgs),
    Cmd("nav.open", kNavOpenArgs),
};

const std::array<CommandSpec, 6> kHistoryCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("history.subscribe", kNoArgs, true),
    Cmd("history.list", kHistoryListArgs),
    Cmd("history.removeUrl", kHistoryRemoveArgs),
    Cmd("history.clear", kNoArgs),
    Cmd("nav.open", kNavOpenArgs),
};

// No "open the file": TorFox only ever shows a download in its folder.
const std::array<CommandSpec, 7> kDownloadsCommands = {
    Cmd("appearance.subscribe", kNoArgs, true),
    Cmd("downloads.subscribe", kNoArgs, true),
    Cmd("downloads.list", kNoArgs),
    Cmd("downloads.cancel", kDownloadIdArgs),
    Cmd("downloads.reveal", kDownloadIdArgs),
    Cmd("downloads.remove", kDownloadIdArgs),
    Cmd("downloads.clear", kNoArgs),
};

const std::array<PageSpec, 7> kPages = {
    PageSpec{"shell", kShellCommands},
    PageSpec{"newtab", kNewTabCommands},
    PageSpec{"settings", kSettingsCommands},
    PageSpec{"bookmarks", kBookmarksCommands},
    PageSpec{"history", kHistoryCommands},
    PageSpec{"downloads", kDownloadsCommands},
    PageSpec{"suggest", kSuggestCommands},
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
