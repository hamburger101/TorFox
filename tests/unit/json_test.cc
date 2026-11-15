// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/json.h"

#include <string>

#include "gtest/gtest.h"

namespace torfox {
namespace {

TEST(Json, ParsesValidObject) {
  auto v = ParseJson(R"({"a": 1, "b": [true, "x"]})", 1024, 4);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->is_object());
  EXPECT_EQ(GetInt(*v, "a"), 1);
}

TEST(Json, RejectsInvalidInput) {
  EXPECT_FALSE(ParseJson("", 1024, 4).has_value());
  EXPECT_FALSE(ParseJson("{", 1024, 4).has_value());
  EXPECT_FALSE(ParseJson("{} {}", 1024, 4).has_value());
  EXPECT_FALSE(ParseJson("{\"a\":1,}", 1024, 4).has_value());
  EXPECT_FALSE(ParseJson("{/*c*/}", 1024, 4).has_value());
  EXPECT_FALSE(ParseJson("{\"a\": NaN}", 1024, 4).has_value());
  // Invalid UTF-8 inside a string.
  EXPECT_FALSE(ParseJson("{\"a\": \"\xC3\x28\"}", 1024, 4).has_value());
  // Overlong encoding of '/'.
  EXPECT_FALSE(ParseJson("{\"a\": \"\xC0\xAF\"}", 1024, 4).has_value());
}

TEST(Json, EnforcesSizeLimit) {
  std::string big = "\"" + std::string(100, 'x') + "\"";
  EXPECT_TRUE(ParseJson(big, big.size(), 4).has_value());
  EXPECT_FALSE(ParseJson(big, big.size() - 1, 4).has_value());
}

TEST(Json, EnforcesDepthLimit) {
  EXPECT_TRUE(ParseJson("[[[1]]]", 1024, 3).has_value());
  EXPECT_FALSE(ParseJson("[[[[1]]]]", 1024, 3).has_value());
  // Brackets inside strings do not count.
  EXPECT_TRUE(ParseJson(R"(["[[[[[[", "\"[[[["])", 1024, 1).has_value());
}

TEST(Json, DumpNeverAbortsOnInvalidUtf8) {
  Json v = Json::object();
  v["s"] = std::string("ok\xFF\xFE");
  std::string out = DumpJson(v);
  EXPECT_NE(out.find("ok"), std::string::npos);
  EXPECT_EQ(out.find('\xFF'), std::string::npos);
}

TEST(Json, TypedGettersRejectWrongTypes) {
  auto v = ParseJson(R"({"s": "x", "b": true, "i": 3, "f": 1.5, "n": null})", 1024, 2);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(GetString(*v, "s"), "x");
  EXPECT_EQ(GetBool(*v, "b"), true);
  EXPECT_EQ(GetInt(*v, "i"), 3);
  EXPECT_FALSE(GetString(*v, "i").has_value());
  EXPECT_FALSE(GetBool(*v, "s").has_value());
  EXPECT_FALSE(GetInt(*v, "f").has_value());
  EXPECT_FALSE(GetInt(*v, "missing").has_value());
  EXPECT_FALSE(GetString(Json::array(), "s").has_value());
  EXPECT_EQ(GetMember(*v, "missing"), nullptr);
}

}  // namespace
}  // namespace torfox
