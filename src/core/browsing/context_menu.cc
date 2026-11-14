// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/context_menu.h"

#include "src/core/url/nav_input.h"

namespace torfox::browsing {

namespace {

constexpr std::size_t kMaxSearchBytes = 1024;

bool IsSpaceOrControl(unsigned char c) {
  return c <= 0x20 || c == 0x7F;
}

// Whitespace and control runs become single spaces; leading and trailing
// ones are dropped.
std::string Collapse(std::string_view text) {
  std::string out;
  bool pending_space = false;
  for (char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (IsSpaceOrControl(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out += ' ';
      pending_space = false;
    }
    out += ch;
  }
  return out;
}

bool ContainsOnionText(std::string_view text) {
  constexpr std::string_view kOnion = ".onion";
  if (text.size() < kOnion.size()) {
    return false;
  }
  for (std::size_t i = 0; i + kOnion.size() <= text.size(); ++i) {
    bool match = true;
    for (std::size_t k = 0; k < kOnion.size(); ++k) {
      char c = text[i + k];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      if (c != kOnion[k]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<std::string> SelectionSearchUrl(std::string_view selection) {
  const std::string query = Collapse(selection);
  if (query.empty() || query.size() > kMaxSearchBytes) {
    return std::nullopt;
  }
  // Stricter than the address bar: any ".onion" text at all, not only a
  // host-shaped token, keeps the selection away from the search engine.
  if (ContainsOnionText(query) || MentionsOnionHost(query)) {
    return std::nullopt;
  }
  return DuckDuckGoSearchUrl(query);
}

std::string MenuExcerpt(std::string_view text, std::size_t max_chars) {
  const std::string line = Collapse(text);
  std::size_t chars = 0;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if ((c & 0xC0) == 0x80) {
      continue;  // Continuation byte.
    }
    if (chars == max_chars) {
      return line.substr(0, i) + "\xE2\x80\xA6";
    }
    ++chars;
  }
  return line;
}

}  // namespace torfox::browsing
