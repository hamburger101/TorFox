// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/public_suffix.h"

#include <optional>
#include <string_view>

#include "gtest/gtest.h"

namespace torfox::psl {
namespace {

#include "psl_vectors.inc"

// The official test vectors from github.com/publicsuffix/list
// (tests/test_psl.txt), converted to ASCII by scripts/gen_psl.py.
TEST(PublicSuffix, OfficialTestVectors) {
  std::size_t checked = 0;
  for (const PslVector& v : kPslVectors) {
    std::optional<std::string> got = RegistrableDomain(v.host);
    if (v.want) {
      EXPECT_EQ(got, std::optional<std::string>(std::string(*v.want))) << v.host;
    } else {
      EXPECT_EQ(got, std::nullopt) << v.host;
    }
    ++checked;
  }
  EXPECT_GE(checked, 70u);
}

TEST(PublicSuffix, Suffixes) {
  EXPECT_EQ(PublicSuffix("www.example.co.uk"), "co.uk");
  EXPECT_EQ(PublicSuffix("example.com"), "com");
  EXPECT_EQ(PublicSuffix("unlisted"), "unlisted");  // The "*" default rule.
  EXPECT_EQ(PublicSuffix("a..b"), std::nullopt);
  EXPECT_EQ(PublicSuffix(""), std::nullopt);
  EXPECT_EQ(PublicSuffix("example.com."), std::nullopt);  // Empty last label.
}

TEST(PublicSuffix, PrivateSectionIsOptional) {
  // github.io is in the list's PRIVATE section: each user site is its own
  // site, which is what third-party checks need.
  EXPECT_EQ(RegistrableDomain("alice.github.io"), "alice.github.io");
  EXPECT_EQ(RegistrableDomain("alice.github.io", Sections::kIcannOnly), "github.io");
}

TEST(PublicSuffix, KnownTlds) {
  EXPECT_TRUE(IsKnownTld("com"));
  EXPECT_TRUE(IsKnownTld("COM"));
  EXPECT_TRUE(IsKnownTld("uk"));
  EXPECT_TRUE(IsKnownTld("xn--p1ai"));                  // .рф in ASCII form
  EXPECT_TRUE(IsKnownTld("\xD1\x80\xD1\x84"));          // .рф
  EXPECT_FALSE(IsKnownTld("js"));
  EXPECT_FALSE(IsKnownTld("local"));
  // Listed (RFC 7686 special-use). The address bar handles onion input
  // before it ever asks about TLDs.
  EXPECT_TRUE(IsKnownTld("onion"));
  EXPECT_FALSE(IsKnownTld(""));
  EXPECT_FALSE(ListVersion().empty());
}

}  // namespace
}  // namespace torfox::psl
