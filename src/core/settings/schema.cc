// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/settings/schema.h"

#include <array>

#include "src/core/tor/bridges.h"

namespace torfox::settings {

namespace {

constexpr std::array<std::string_view, 8> kSections = {
    "general", "search", "privacy", "downloads", "tor", "appearance", "updates", "about",
};

constexpr std::array<std::string_view, 2> kStartupPages = {"newtab", "restore"};
constexpr std::array<std::string_view, 3> kSearchEngines = {"duckduckgo", "duckduckgo-html",
                                                            "duckduckgo-lite"};
constexpr std::array<std::string_view, 2> kPermission = {"deny", "ask"};
constexpr std::array<std::string_view, 2> kTorStart = {"on-demand", "at-launch"};
constexpr std::array<std::string_view, 3> kBridgeModes = {"none", "builtin", "custom"};
constexpr std::array<std::string_view, 3> kBridgeTypes = {"obfs4", "snowflake", "meek"};
constexpr std::array<std::string_view, 3> kThemes = {"dark", "light", "system"};

constexpr Def Bool(std::string_view key, std::string_view section, Status status, bool def) {
  Def d{};
  d.key = key;
  d.section = section;
  d.type = Type::kBool;
  d.status = status;
  d.default_bool = def;
  return d;
}

template <std::size_t N>
constexpr Def Enum(std::string_view key,
                   std::string_view section,
                   Status status,
                   const std::array<std::string_view, N>& values) {
  Def d{};
  d.key = key;
  d.section = section;
  d.type = Type::kEnum;
  d.status = status;
  d.enum_values = std::span<const std::string_view>(values);
  d.default_string = values[0];  // The first value is always the default.
  return d;
}

constexpr Def String(std::string_view key,
                     std::string_view section,
                     Status status,
                     std::size_t max_length,
                     bool (*check)(std::string_view) = nullptr) {
  Def d{};
  d.check = check;
  d.key = key;
  d.section = section;
  d.type = Type::kString;
  d.status = status;
  d.default_string = "";
  d.max_length = max_length;
  return d;
}

constexpr Status A = Status::kActive;
constexpr Status P = Status::kPlanned;

const std::array<Def, 24> kSettings = {
    // General
    Enum("general.startupPage", "general", A, kStartupPages),
    // Search Engine
    Enum("search.engine", "search", A, kSearchEngines),
    // Privacy & Security
    Bool("privacy.httpsFirst", "privacy", A, true),
    Bool("privacy.blockTrackers", "privacy", A, true),
    Bool("privacy.blockThirdPartyCookies", "privacy", A, true),
    Bool("privacy.blockPopups", "privacy", A, true),
    Bool("privacy.useSystemProxy", "privacy", A, false),
    Bool("privacy.clearOnExit.history", "privacy", A, false),
    Bool("privacy.clearOnExit.cookies", "privacy", A, false),
    Bool("privacy.clearOnExit.cache", "privacy", A, false),
    Enum("permissions.camera", "privacy", A, kPermission),
    Enum("permissions.microphone", "privacy", A, kPermission),
    Enum("permissions.clipboard", "privacy", A, kPermission),
    // Downloads
    Bool("downloads.askBeforeDownload", "downloads", A, true),
    Bool("downloads.disableDownloads", "downloads", A, false),
    // Tor Network
    Enum("tor.startMode", "tor", A, kTorStart),
    Enum("tor.bridges.mode", "tor", A, kBridgeModes),
    Enum("tor.bridges.builtinType", "tor", A, kBridgeTypes),
    String("tor.bridges.custom", "tor", A, 16384, &tor::BridgeTextIsValid),
    // Appearance
    Enum("appearance.theme", "appearance", A, kThemes),
    Bool("appearance.showBookmarksBar", "appearance", A, false),
    // Updates
    Bool("updates.autoCheck", "updates", P, true),
    Bool("updates.notify", "updates", P, true),
    Bool("updates.enabled", "updates", P, true),
};

bool IsValidUtf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    unsigned int cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > s.size()) {
      return false;
    }
    for (std::size_t k = 1; k < len; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

}  // namespace

std::span<const Def> AllSettings() {
  return std::span<const Def>(kSettings);
}

std::span<const std::string_view> AllSections() {
  return std::span<const std::string_view>(kSections);
}

const Def* Find(std::string_view key) {
  for (const Def& d : kSettings) {
    if (d.key == key) {
      return &d;
    }
  }
  return nullptr;
}

bool IsValid(const Def& def, const Json& value) {
  switch (def.type) {
    case Type::kBool:
      return value.is_boolean();
    case Type::kEnum: {
      if (!value.is_string()) {
        return false;
      }
      const std::string& s = value.get_ref<const std::string&>();
      for (std::string_view allowed : def.enum_values) {
        if (s == allowed) {
          return true;
        }
      }
      return false;
    }
    case Type::kString: {
      if (!value.is_string()) {
        return false;
      }
      const std::string& s = value.get_ref<const std::string&>();
      if (s.size() > def.max_length || !IsValidUtf8(s)) {
        return false;
      }
      for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        // Control characters other than newline and tab are never valid.
        if (u < 0x20 && c != '\n' && c != '\t' && c != '\r') {
          return false;
        }
        if (u == 0x7F) {
          return false;
        }
      }
      return !def.check || def.check(s);
    }
  }
  return false;
}

Json DefaultValue(const Def& def) {
  switch (def.type) {
    case Type::kBool:
      return Json(def.default_bool);
    case Type::kEnum:
    case Type::kString:
      return Json(std::string(def.default_string));
  }
  return Json();
}

std::string_view TypeName(Type type) {
  switch (type) {
    case Type::kBool:
      return "bool";
    case Type::kEnum:
      return "enum";
    case Type::kString:
      return "string";
  }
  return "unknown";
}

std::string_view StatusName(Status status) {
  return status == Status::kActive ? "active" : "planned";
}

}  // namespace torfox::settings
