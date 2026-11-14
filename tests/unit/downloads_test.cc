// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <set>
#include <string>

#include "gtest/gtest.h"
#include "src/core/downloads/policy.h"
#include "src/core/util/utf8.h"

namespace torfox::downloads {
namespace {

TEST(DownloadPolicy, DisableOverridesAsk) {
  EXPECT_EQ(Decide(/*disable=*/true, /*ask=*/true), Decision::kBlock);
  EXPECT_EQ(Decide(true, false), Decision::kBlock);
  EXPECT_EQ(Decide(false, true), Decision::kAsk);
  EXPECT_EQ(Decide(false, false), Decision::kAllow);
}

TEST(DownloadFileName, KeepsOrdinaryNames) {
  EXPECT_EQ(SafeFileName("report.pdf"), "report.pdf");
  EXPECT_EQ(SafeFileName("Résumé 2026.docx"), "Résumé 2026.docx");
  EXPECT_EQ(SafeFileName("archive.tar.gz"), "archive.tar.gz");
  EXPECT_EQ(SafeFileName("\xE6\x97\xA5\xE6\x9C\xAC.txt"), "\xE6\x97\xA5\xE6\x9C\xAC.txt");
}

TEST(DownloadFileName, KeepsOnlyTheLastPathComponent) {
  EXPECT_EQ(SafeFileName("../../Windows/System32/evil.dll"), "evil.dll");
  EXPECT_EQ(SafeFileName("..\\..\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\x.lnk"), "x.lnk");
  EXPECT_EQ(SafeFileName("C:\\Users\\x\\a.txt"), "a.txt");
  EXPECT_EQ(SafeFileName("/etc/passwd"), "passwd");
  EXPECT_EQ(SafeFileName("dir/"), "download");
  EXPECT_EQ(SafeFileName(".."), "download");
}

TEST(DownloadFileName, ReplacesReservedAndControlCharacters) {
  EXPECT_EQ(SafeFileName("a<b>c:d\"e|f?g*h.txt"), "a_b_c_d_e_f_g_h.txt");
  EXPECT_EQ(SafeFileName(std::string("a\0b.txt", 7)), "a_b.txt");
  EXPECT_EQ(SafeFileName("tab\there\r\n.txt"), "tab_here__.txt");
  EXPECT_EQ(SafeFileName("c1\xC2\x85.txt"), "c1_.txt");  // U+0085 (C1 control).
  EXPECT_EQ(SafeFileName("bad\xFF\xFEutf8.txt"), "bad__utf8.txt");
  EXPECT_EQ(SafeFileName("over\xC0\xAFlong.txt"), "over__long.txt");  // Overlong "/".
}

TEST(DownloadFileName, DropsDirectionalAndInvisibleCharacters) {
  // "invoice\u202Etxt.exe" displays as "invoiceexe.txt".
  EXPECT_EQ(SafeFileName("invoice\xE2\x80\xAEtxt.exe"), "invoicetxt.exe");
  EXPECT_EQ(SafeFileName("a\xE2\x81\xA6" "b\xE2\x81\xA9.pdf"), "ab.pdf");     // U+2066, U+2069.
  EXPECT_EQ(SafeFileName("zero\xE2\x80\x8Bwidth.txt"), "zerowidth.txt");      // U+200B.
  EXPECT_EQ(SafeFileName("\xEF\xBB\xBF" "bom.txt"), "bom.txt");               // U+FEFF.
}

TEST(DownloadFileName, TrimsDotsAndSpaces) {
  EXPECT_EQ(SafeFileName(".bashrc"), "bashrc");
  EXPECT_EQ(SafeFileName("...hidden.txt"), "hidden.txt");
  EXPECT_EQ(SafeFileName("file.exe. . ."), "file.exe");  // Windows would strip these.
  EXPECT_EQ(SafeFileName("  spaced.txt  "), "spaced.txt");
  EXPECT_EQ(SafeFileName(""), "download");
  EXPECT_EQ(SafeFileName("   "), "download");
  EXPECT_EQ(SafeFileName("\xE2\x80\xAE"), "download");
}

TEST(DownloadFileName, PrefixesWindowsDeviceNames) {
  for (const char* name : {"CON", "con.txt", "Prn.log", "aux", "NUL.tar.gz", "COM1", "com9.bin",
                           "LPT1.txt", "lpt0", "COM\xC2\xB9.txt", "CONIN$", "conout$.x", "CON .txt"}) {
    EXPECT_EQ(SafeFileName(name)[0], '_') << name;
  }
  for (const char* name : {"CONSOLE.txt", "COM10", "LPT", "nulled.txt", "auxiliary"}) {
    EXPECT_NE(SafeFileName(name)[0], '_') << name;
  }
}

TEST(DownloadFileName, LimitsLengthAndKeepsTheExtension) {
  const std::string name = SafeFileName(std::string(500, 'a') + ".pdf");
  EXPECT_EQ(name.size(), kMaxNameBytes);
  EXPECT_EQ(name.substr(name.size() - 4), ".pdf");
  // Multi-byte characters are never cut in half.
  std::string wide;
  for (int i = 0; i < 300; ++i) {
    wide += "\xE2\x82\xAC";  // U+20AC, three bytes.
  }
  const std::string cut = SafeFileName(wide + ".txt");
  EXPECT_LE(cut.size(), kMaxNameBytes);
  EXPECT_TRUE(utf8::IsValid(cut));
  EXPECT_EQ(cut.substr(cut.size() - 4), ".txt");
  // A very long "extension" is not kept.
  const std::string odd = SafeFileName("x." + std::string(400, 'b'));
  EXPECT_EQ(odd.size(), kMaxNameBytes);
}

TEST(DownloadFileName, ResultIsAlwaysSafe) {
  // Every byte value, in every position: the result is never empty, never
  // valid as a path with separators, never starts with a dot.
  for (int b = 0; b < 256; ++b) {
    for (const std::string& pattern : {std::string(1, static_cast<char>(b)),
                                       "a" + std::string(1, static_cast<char>(b)) + "b.txt",
                                       std::string(1, static_cast<char>(b)) + ".txt"}) {
      const std::string name = SafeFileName(pattern);
      EXPECT_FALSE(name.empty());
      EXPECT_EQ(name.find_first_of("/\\:<>\"|?*"), std::string::npos) << b;
      EXPECT_NE(name.front(), '.') << b;
      EXPECT_NE(name.back(), '.') << b;
      EXPECT_NE(name.back(), ' ') << b;
      EXPECT_TRUE(utf8::IsValid(name)) << b;
      for (char c : name) {
        EXPECT_GE(static_cast<unsigned char>(c), 0x20) << b;
      }
    }
  }
}

TEST(DownloadPath, NeverOverwrites) {
  std::set<std::filesystem::path> existing = {std::filesystem::path("d") / "a.txt",
                                              std::filesystem::path("d") / "a (1).txt"};
  auto exists = [&](const std::filesystem::path& p) { return existing.count(p) > 0; };
  EXPECT_EQ(*UniquePath("d", "b.txt", exists), std::filesystem::path("d") / "b.txt");
  EXPECT_EQ(*UniquePath("d", "a.txt", exists), std::filesystem::path("d") / "a (2).txt");
  existing.insert(std::filesystem::path("d") / "noext");
  EXPECT_EQ(*UniquePath("d", "noext", exists), std::filesystem::path("d") / "noext (1)");
  EXPECT_FALSE(UniquePath("d", "x", [](const std::filesystem::path&) { return true; }));
  const auto unicode = UniquePath("d", "\xC3\xA9t\xC3\xA9.txt", exists);
  ASSERT_TRUE(unicode);
  EXPECT_EQ(unicode->filename().u8string(), u8"\u00e9t\u00e9.txt");
}

TEST(DownloadPath, StaysWithinThePathLimit) {
  auto none = [](const std::filesystem::path&) { return false; };
  const std::filesystem::path dir(std::string(100, 'd'));
  const std::string stem(196, 'n');
  const std::size_t reserve = kPartialSuffix.size();
  const auto p = UniquePath(dir, stem + ".txt", none, reserve);
  ASSERT_TRUE(p);
  EXPECT_LE(p->native().size() + reserve, kMaxPathLength);
  EXPECT_EQ(p->extension(), ".txt");
  const std::string name = p->filename().string();
  EXPECT_EQ(name.substr(0, name.size() - 4), stem.substr(0, name.size() - 4));
  if (dir.native().size() + 1 + stem.size() + 4 + reserve <= kMaxPathLength) {
    EXPECT_EQ(name, stem + ".txt");  // Fits as it is.
  } else {
    EXPECT_EQ(p->native().size() + reserve, kMaxPathLength);  // As long as it can be.
  }

  // Numbered names fit too.
  const auto second = UniquePath(dir, stem + ".txt",
                                 [&](const std::filesystem::path& q) { return q == *p; }, reserve);
  ASSERT_TRUE(second);
  EXPECT_LE(second->native().size() + reserve, kMaxPathLength);
  EXPECT_NE(second->filename().string().find(" (1).txt"), std::string::npos);

  // Shortening cuts whole characters and drops trailing dots and spaces.
  std::string accents;
  for (int i = 0; i < 99; ++i) {
    accents += i % 10 == 9 ? " " : "\xC3\xA9";
  }
  const auto unicode = UniquePath(std::filesystem::path(std::string(kMaxPathLength - 80, 'd')),
                                  accents + ".pdf", none, reserve);
  ASSERT_TRUE(unicode);
  const std::u8string u = unicode->filename().u8string();
  const std::string bytes(u.begin(), u.end());
  EXPECT_TRUE(utf8::IsValid(bytes));
  EXPECT_LE(unicode->native().size() + reserve, kMaxPathLength);
  EXPECT_EQ(bytes.substr(bytes.size() - 4), ".pdf");
  EXPECT_NE(bytes[bytes.size() - 5], ' ');

  // A folder too deep for any name: no path at all.
  EXPECT_FALSE(UniquePath(std::filesystem::path(std::string(kMaxPathLength, 'd')), "a.txt", none,
                          reserve));
}

TEST(DownloadTypes, FlagsProgramsAndScripts) {
  for (const char* name : {"setup.exe", "SETUP.EXE", "x.msi", "run.bat", "a.ps1", "s.vbs", "link.lnk",
                           "page.url", "t.scf", "app.dmg", "a.pkg", "x.command", "disk.iso",
                           "y.js", "z.hta", "q.jar"}) {
    EXPECT_TRUE(IsDangerousType(name)) << name;
  }
  for (const char* name : {"doc.pdf", "photo.jpg", "notes.txt", "exe", "archive.zip", "a.exe.txt"}) {
    EXPECT_FALSE(IsDangerousType(name)) << name;
  }
}

TEST(DownloadText, FormatsSizes) {
  EXPECT_EQ(FormatSize(-1), "Unknown size");
  EXPECT_EQ(FormatSize(0), "0 bytes");
  EXPECT_EQ(FormatSize(1), "1 byte");
  EXPECT_EQ(FormatSize(1023), "1023 bytes");
  EXPECT_EQ(FormatSize(1536), "1.5 KB");
  EXPECT_EQ(FormatSize(12 * 1024 * 1024), "12.0 MB");
  EXPECT_EQ(FormatSize(int64_t{3} * 1024 * 1024 * 1024 / 2), "1.5 GB");
}

TEST(DownloadText, ShowsOnlyTheOrigin) {
  EXPECT_EQ(DisplayOrigin("https://Example.com/files/a.zip?x=1"), "https://example.com");
  EXPECT_EQ(DisplayOrigin("http://127.0.0.1:8080/a"), "http://127.0.0.1:8080");
  EXPECT_EQ(DisplayOrigin("blob:https://site.example/1234-5678"), "https://site.example");
  EXPECT_EQ(DisplayOrigin("https://user:secret@host.example/x"), "https://host.example");
  EXPECT_EQ(DisplayOrigin("data:text/plain,hello"), "");
  EXPECT_EQ(DisplayOrigin("file:///C:/x"), "");
  EXPECT_EQ(DisplayOrigin("https://"), "");
}

TEST(Utf8, RoundTripsAndRejectsMalformedInput) {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  EXPECT_EQ(utf8::Encode(utf8::Decode(text, U'?')), text);
  EXPECT_TRUE(utf8::IsValid(text));
  EXPECT_FALSE(utf8::IsValid("\xED\xA0\x80"));      // Surrogate.
  EXPECT_FALSE(utf8::IsValid("\xF4\x90\x80\x80"));  // Above U+10FFFF.
  EXPECT_FALSE(utf8::IsValid("\xE2\x82"));          // Truncated.
  EXPECT_EQ(utf8::Decode("\xE2\x82", U'?'), U"??");
}

}  // namespace
}  // namespace torfox::downloads
