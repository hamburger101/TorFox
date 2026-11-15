// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What's new is shown once, after an update: only when the profile's record
// names an older version.

#include <string>

#include "gtest/gtest.h"
#include "src/core/update/whats_new.h"

namespace torfox::update {
namespace {

TEST(WhatsNew, ShownOnlyAfterAnUpdate) {
  EXPECT_TRUE(ShouldShowWhatsNew("0.1.0", "0.1.1"));
  EXPECT_TRUE(ShouldShowWhatsNew("0.1.9\n", "0.2.0"));
  EXPECT_TRUE(ShouldShowWhatsNew("  1.2.3\r\n", "2.0.0"));
  EXPECT_TRUE(ShouldShowWhatsNew("0.9.9", "0.10.0"));  // Numbers, not text.
  // The same version, a newer one (TorFox was downgraded), or a new profile.
  EXPECT_FALSE(ShouldShowWhatsNew("0.1.1", "0.1.1"));
  EXPECT_FALSE(ShouldShowWhatsNew("0.2.0", "0.1.1"));
  EXPECT_FALSE(ShouldShowWhatsNew("", "0.1.1"));
}

TEST(WhatsNew, AnythingButAVersionIsIgnored) {
  for (const char* bad : {"garbage", "0.1", "0.1.0.1", "v0.1.0", "0.1.0 extra", "-1.0.0",
                          "0.1.0\n0.0.1", "\x00\x01", "01.1.0x"}) {
    EXPECT_FALSE(ShouldShowWhatsNew(bad, "9.9.9")) << bad;
    EXPECT_FALSE(RecordedVersion(bad).has_value()) << bad;
  }
  // Longer than any record TorFox writes.
  EXPECT_FALSE(ShouldShowWhatsNew(std::string(100, ' ') + "0.1.0", "9.9.9"));
  // TorFox's own version must be one too.
  EXPECT_FALSE(ShouldShowWhatsNew("0.1.0", "dev"));
}

TEST(WhatsNew, RecordedVersionIgnoresSurroundingSpace) {
  EXPECT_EQ(RecordedVersion("0.1.0\n").value_or(""), "0.1.0");
  EXPECT_EQ(RecordedVersion(" \t12.0.3\r\n").value_or(""), "12.0.3");
  EXPECT_FALSE(RecordedVersion(" \n ").has_value());
}

TEST(WhatsNew, HandoffRecordTellsWhetherTheUpdateInstalled) {
  EXPECT_EQ(CheckHandoff("0.2.0\n", "0.2.0"), HandoffOutcome::kInstalled);
  EXPECT_EQ(CheckHandoff("0.2.0\n", "0.1.0"), HandoffOutcome::kNotInstalled);
  EXPECT_EQ(CheckHandoff("0.10.0", "0.9.0"), HandoffOutcome::kNotInstalled);  // Numbers, not text.
  // Installed again by hand since: says nothing.
  EXPECT_EQ(CheckHandoff("0.2.0\n", "0.3.0"), HandoffOutcome::kNone);
  // No record, or not a version.
  EXPECT_EQ(CheckHandoff("", "0.2.0"), HandoffOutcome::kNone);
  EXPECT_EQ(CheckHandoff("v0.2.0", "0.2.0"), HandoffOutcome::kNone);
  EXPECT_EQ(CheckHandoff("0.2.0\n0.2.0\n", "0.2.0"), HandoffOutcome::kNone);
  EXPECT_EQ(CheckHandoff(std::string(100, '1'), "0.2.0"), HandoffOutcome::kNone);
}

TEST(WhatsNew, NotesAreNormalizedText) {
  EXPECT_EQ(NormalizeNotes("One.\r\nTwo.\rThree.\n"), "One.\nTwo.\nThree.\n");
  EXPECT_EQ(NormalizeNotes("<b>kept as text</b>"), "<b>kept as text</b>");
  EXPECT_EQ(NormalizeNotes("caf\xC3\xA9"), "caf\xC3\xA9");
  // Not UTF-8, or too long: none at all rather than something broken.
  EXPECT_EQ(NormalizeNotes("bad \xC3\x28"), "");
  EXPECT_EQ(NormalizeNotes(std::string(kMaxBundledNotesBytes + 1, 'a')), "");
  EXPECT_EQ(NormalizeNotes(std::string(kMaxBundledNotesBytes, 'a')).size(), kMaxBundledNotesBytes);
}

}  // namespace
}  // namespace torfox::update
