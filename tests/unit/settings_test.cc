// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <filesystem>
#include <set>
#include <string>

#include "gtest/gtest.h"
#include "src/core/settings/schema.h"
#include "src/core/settings/store.h"
#include "src/core/util/file_io.h"
#include "tests/unit/test_util.h"

namespace torfox::settings {
namespace {

namespace fs = std::filesystem;

std::string ReadAll(const fs::path& p) {
  return ReadFileLimited(p, 1 << 20).data;
}

void WriteRaw(const fs::path& p, const std::string& data) {
  ASSERT_TRUE(WriteFileAtomically(p, data));
}

// ----- Schema ---------------------------------------------------------------

TEST(SettingsSchema, KeysAreUniqueAndDefaultsValid) {
  std::set<std::string_view> keys;
  for (const Def& d : AllSettings()) {
    EXPECT_TRUE(keys.insert(d.key).second) << "duplicate key " << d.key;
    EXPECT_TRUE(IsValid(d, DefaultValue(d))) << d.key;
    bool section_known = false;
    for (std::string_view s : AllSections()) {
      section_known |= (s == d.section);
    }
    EXPECT_TRUE(section_known) << d.key;
  }
}

TEST(SettingsSchema, SaferDefaults) {
  // These defaults are requirements, not preferences.
  auto def = [](std::string_view key) { return DefaultValue(*Find(key)); };
  EXPECT_EQ(def("downloads.askBeforeDownload"), Json(true));
  EXPECT_EQ(def("downloads.disableDownloads"), Json(false));
  EXPECT_EQ(def("search.engine"), Json("duckduckgo"));
  EXPECT_EQ(def("privacy.httpsFirst"), Json(true));
  EXPECT_EQ(def("privacy.blockTrackers"), Json(true));
  EXPECT_EQ(def("privacy.blockThirdPartyCookies"), Json(true));
  EXPECT_EQ(def("privacy.blockPopups"), Json(true));
  for (auto p : {"camera", "microphone", "location", "notifications", "clipboard"}) {
    EXPECT_EQ(def(std::string("permissions.") + p), Json("deny")) << p;
  }
  EXPECT_EQ(def("appearance.theme"), Json("dark"));
  EXPECT_EQ(def("tor.bridges.mode"), Json("none"));
  EXPECT_EQ(def("updates.autoCheck"), Json(true));
}

TEST(SettingsSchema, OnlyImplementedSettingsAreActive) {
  // Stage 1 implements the theme only. Every other setting must stay
  // "planned" until its feature exists and is tested.
  for (const Def& d : AllSettings()) {
    if (d.key == "appearance.theme") {
      EXPECT_EQ(d.status, Status::kActive);
    } else {
      EXPECT_EQ(d.status, Status::kPlanned) << d.key;
    }
  }
}

TEST(SettingsSchema, ValidatesTypesAndConstraints) {
  const Def* theme = Find("appearance.theme");
  ASSERT_NE(theme, nullptr);
  EXPECT_TRUE(IsValid(*theme, Json("light")));
  EXPECT_FALSE(IsValid(*theme, Json("Light")));
  EXPECT_FALSE(IsValid(*theme, Json("purple")));
  EXPECT_FALSE(IsValid(*theme, Json(1)));
  EXPECT_FALSE(IsValid(*theme, Json(nullptr)));

  const Def* ask = Find("downloads.askBeforeDownload");
  EXPECT_TRUE(IsValid(*ask, Json(false)));
  EXPECT_FALSE(IsValid(*ask, Json("false")));
  EXPECT_FALSE(IsValid(*ask, Json(0)));

  const Def* custom = Find("tor.bridges.custom");
  EXPECT_TRUE(IsValid(*custom, Json("obfs4 192.0.2.1:443 ABCD cert=x iat-mode=0\n")));
  EXPECT_FALSE(IsValid(*custom, Json(std::string(custom->max_length + 1, 'a'))));
  EXPECT_FALSE(IsValid(*custom, Json(std::string("a\x01b"))));
  EXPECT_FALSE(IsValid(*custom, Json(std::string("bad\xFF"))));

  EXPECT_EQ(Find("no.such.key"), nullptr);
}

// ----- Store ----------------------------------------------------------------

TEST(SettingsStore, MissingFileUsesDefaults) {
  testing::ScopedTempDir dir;
  Store store(dir.path() / "settings.json");
  LoadResult r = store.Load();
  EXPECT_EQ(r.outcome, LoadOutcome::kMissing);
  EXPECT_EQ(store.GetString("appearance.theme"), "dark");
  EXPECT_TRUE(store.GetBool("downloads.askBeforeDownload"));
}

TEST(SettingsStore, SetRejectsInvalidValues) {
  testing::ScopedTempDir dir;
  Store store(dir.path() / "settings.json");
  EXPECT_EQ(store.Set("appearance.theme", Json("light")), SetResult::kOk);
  EXPECT_EQ(store.Set("appearance.theme", Json("light")), SetResult::kUnchanged);
  EXPECT_EQ(store.Set("appearance.theme", Json("neon")), SetResult::kInvalidValue);
  EXPECT_EQ(store.Set("appearance.theme", Json(true)), SetResult::kInvalidValue);
  EXPECT_EQ(store.Set("nope", Json(true)), SetResult::kUnknownKey);
  EXPECT_EQ(store.GetString("appearance.theme"), "light");
}

TEST(SettingsStore, SaveLoadRoundTrip) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "nested" / "settings.json";
  {
    Store store(file);
    store.Load();
    ASSERT_EQ(store.Set("appearance.theme", Json("system")), SetResult::kOk);
    ASSERT_EQ(store.Set("downloads.askBeforeDownload", Json(false)), SetResult::kOk);
    ASSERT_TRUE(store.Save());
  }
  EXPECT_FALSE(fs::exists(fs::path(file.string() + ".tmp")));
  Store reloaded(file);
  LoadResult r = reloaded.Load();
  EXPECT_EQ(r.outcome, LoadOutcome::kLoaded);
  EXPECT_EQ(reloaded.GetString("appearance.theme"), "system");
  EXPECT_FALSE(reloaded.GetBool("downloads.askBeforeDownload"));
  EXPECT_FALSE(reloaded.GetBool("downloads.disableDownloads"));
}

TEST(SettingsStore, OnlyNonDefaultValuesAreWritten) {
  testing::ScopedTempDir dir;
  Store store(dir.path() / "settings.json");
  store.Load();
  store.Set("appearance.theme", Json("light"));
  store.Set("appearance.theme", Json("dark"));  // Back to the default.
  store.Set("privacy.blockPopups", Json(false));
  auto parsed = ParseJson(store.Serialize(), 1 << 16, 4);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(GetInt(*parsed, "format"), kFormatVersion);
  const Json* s = GetMember(*parsed, "settings");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->size(), 1u);
  EXPECT_EQ(GetBool(*s, "privacy.blockPopups"), false);
}

TEST(SettingsStore, CorruptFileIsBackedUpAndReset) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "settings.json";
  WriteRaw(file, "{ this is not json");
  Store store(file);
  LoadResult r = store.Load();
  EXPECT_EQ(r.outcome, LoadOutcome::kCorruptReset);
  EXPECT_FALSE(r.notes.empty());
  ASSERT_FALSE(r.backup_path.empty());
  EXPECT_EQ(ReadAll(r.backup_path), "{ this is not json");
  EXPECT_FALSE(fs::exists(file));
  EXPECT_EQ(store.GetString("appearance.theme"), "dark");
}

TEST(SettingsStore, WrongShapeIsCorrupt) {
  for (const char* bad : {"[]", "{}", "{\"format\":1}", "{\"settings\":{}}",
                          "{\"format\":\"1\",\"settings\":{}}", "{\"format\":1,\"settings\":[]}",
                          "null", "\"x\""}) {
    testing::ScopedTempDir dir;
    const fs::path file = dir.path() / "settings.json";
    WriteRaw(file, bad);
    Store store(file);
    EXPECT_EQ(store.Load().outcome, LoadOutcome::kCorruptReset) << bad;
  }
}

TEST(SettingsStore, TooLargeFileIsReset) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "settings.json";
  WriteRaw(file, std::string(kMaxFileBytes + 1, ' '));
  Store store(file);
  EXPECT_EQ(store.Load().outcome, LoadOutcome::kCorruptReset);
}

TEST(SettingsStore, InvalidAndUnknownEntriesAreDroppedIndividually) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "settings.json";
  WriteRaw(file, R"({"format":1,"settings":{
      "appearance.theme":"light",
      "downloads.disableDownloads":"yes",
      "permissions.camera":"allow",
      "evil.key":true}})");
  Store store(file);
  LoadResult r = store.Load();
  EXPECT_EQ(r.outcome, LoadOutcome::kRepaired);
  EXPECT_EQ(r.notes.size(), 3u);
  EXPECT_EQ(store.GetString("appearance.theme"), "light");          // Kept.
  EXPECT_FALSE(store.GetBool("downloads.disableDownloads"));         // Default.
  EXPECT_EQ(store.GetString("permissions.camera"), "deny");          // Safer default.
  EXPECT_EQ(store.Serialize().find("evil.key"), std::string::npos);  // Not re-written.
}

TEST(SettingsStore, OtherFormatVersionKeepsKnownValues) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "settings.json";
  WriteRaw(file, R"({"format":2,"settings":{"appearance.theme":"light"}})");
  Store store(file);
  EXPECT_EQ(store.Load().outcome, LoadOutcome::kRepaired);
  EXPECT_EQ(store.GetString("appearance.theme"), "light");
}

TEST(SettingsStore, FailedSaveLeavesNoTempFile) {
  testing::ScopedTempDir dir;
  // The target path is a directory, so the final rename must fail.
  const fs::path file = dir.path() / "settings.json";
  fs::create_directories(file / "sub");
  Store store(file);
  store.Set("appearance.theme", Json("light"));
  EXPECT_FALSE(store.Save());
  EXPECT_TRUE(fs::is_directory(file / "sub"));
  EXPECT_FALSE(fs::exists(fs::path(file.string() + ".tmp")));
}

TEST(SettingsStore, SaveReplacesExistingFileCompletely) {
  testing::ScopedTempDir dir;
  const fs::path file = dir.path() / "settings.json";
  WriteRaw(file, std::string(4096, 'x'));
  Store store(file);
  store.Set("appearance.theme", Json("light"));
  ASSERT_TRUE(store.Save());
  EXPECT_EQ(ReadAll(file), store.Serialize());
}

}  // namespace
}  // namespace torfox::settings
