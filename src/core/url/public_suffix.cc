// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/url/public_suffix.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace torfox::psl {

namespace {

#include "psl_data.inc"

template <std::size_t N>
bool Contains(const std::string_view (&table)[N], std::string_view key) {
  return std::binary_search(std::begin(table), std::end(table), key);
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// Splits |host| into labels; empty if any label is empty.
std::vector<std::string_view> Labels(std::string_view host) {
  std::vector<std::string_view> labels;
  std::size_t start = 0;
  while (true) {
    std::size_t dot = host.find('.', start);
    std::string_view label = host.substr(start, dot == std::string_view::npos ? dot : dot - start);
    if (label.empty()) {
      return {};
    }
    labels.push_back(label);
    if (dot == std::string_view::npos) {
      return labels;
    }
    start = dot + 1;
  }
}

bool IsRule(std::string_view name, Sections sections) {
  return Contains(kPslIcannRules, name) ||
         (sections == Sections::kIcannAndPrivate && Contains(kPslPrivateRules, name));
}

// Number of labels of |labels| that form the public suffix (>= 1).
std::size_t SuffixLabelCount(const std::string& host,
                             const std::vector<std::string_view>& labels,
                             Sections sections) {
  const std::size_t n = labels.size();
  std::size_t best = 1;  // The default rule "*".
  for (std::size_t i = 0; i < n; ++i) {
    // |candidate| is labels[i..n), |parent| is labels[i+1..n).
    const std::size_t offset = static_cast<std::size_t>(labels[i].data() - host.data());
    const std::string_view candidate = std::string_view(host).substr(offset);
    if (Contains(kPslExceptions, candidate)) {
      // An exception rule wins over everything; its public suffix is the
      // rule minus its leftmost label.
      return n - i - 1;
    }
    const std::size_t count = n - i;
    if (count <= best) {
      continue;
    }
    if (IsRule(candidate, sections)) {
      best = count;
      continue;
    }
    if (i + 1 < n) {
      const std::string wildcard = "*." + std::string(std::string_view(host).substr(
                                              static_cast<std::size_t>(labels[i + 1].data() - host.data())));
      if (IsRule(wildcard, sections)) {
        best = count;
      }
    }
  }
  return best;
}

}  // namespace

std::optional<std::string> PublicSuffix(std::string_view raw, Sections sections) {
  const std::string host = LowerAscii(raw);
  const std::vector<std::string_view> labels = Labels(host);
  if (labels.empty()) {
    return std::nullopt;
  }
  const std::size_t count = SuffixLabelCount(host, labels, sections);
  const std::size_t first = labels.size() - count;
  return host.substr(static_cast<std::size_t>(labels[first].data() - host.data()));
}

std::optional<std::string> RegistrableDomain(std::string_view raw, Sections sections) {
  const std::string host = LowerAscii(raw);
  const std::vector<std::string_view> labels = Labels(host);
  if (labels.empty()) {
    return std::nullopt;
  }
  const std::size_t count = SuffixLabelCount(host, labels, sections);
  if (count >= labels.size()) {
    return std::nullopt;
  }
  const std::size_t first = labels.size() - count - 1;
  return host.substr(static_cast<std::size_t>(labels[first].data() - host.data()));
}

bool IsKnownTld(std::string_view label) {
  // Unicode labels are matched as given (lower-casing applies to ASCII
  // only; the list's Unicode TLDs have no upper-case forms).
  return !label.empty() && Contains(kPslTlds, LowerAscii(label));
}

std::string_view ListVersion() {
  return kPslCommit;
}

}  // namespace torfox::psl
