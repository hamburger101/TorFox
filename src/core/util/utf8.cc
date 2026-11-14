// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/util/utf8.h"

namespace torfox::utf8 {

namespace {

// Decodes one sequence at |i|; returns false (and advances by one byte) if it
// is malformed.
bool Next(std::string_view s, std::size_t& i, char32_t& out) {
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    out = b0;
    ++i;
    return true;
  }
  int len = 0;
  char32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    out = b0 & 0x1F;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    out = b0 & 0x0F;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    out = b0 & 0x07;
    min = 0x10000;
  } else {
    ++i;
    return false;
  }
  if (i + len > s.size()) {
    ++i;
    return false;
  }
  for (int k = 1; k < len; ++k) {
    const unsigned char b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) {
      ++i;
      return false;
    }
    out = (out << 6) | (b & 0x3F);
  }
  if (out < min || out > 0x10FFFF || (out >= 0xD800 && out <= 0xDFFF)) {
    ++i;
    return false;
  }
  i += len;
  return true;
}

}  // namespace

std::u32string Decode(std::string_view text, char32_t replacement) {
  std::u32string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t c = 0;
    out.push_back(Next(text, i, c) ? c : replacement);
  }
  return out;
}

std::string Encode(std::u32string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char32_t c : text) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
      c = 0xFFFD;
    }
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

bool IsValid(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t c = 0;
    if (!Next(text, i, c)) {
      return false;
    }
  }
  return true;
}

}  // namespace torfox::utf8
