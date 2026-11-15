// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/update/whats_new.h"

#include "src/core/update/release.h"
#include "src/core/util/utf8.h"

namespace torfox::update {

std::optional<std::string> RecordedVersion(std::string_view record) {
  if (record.size() > kMaxLastVersionBytes) {
    return std::nullopt;
  }
  const std::size_t start = record.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t end = record.find_last_not_of(" \t\r\n");
  const std::string_view text = record.substr(start, end - start + 1);
  if (!ParseVersion(text)) {
    return std::nullopt;
  }
  return std::string(text);
}

bool ShouldShowWhatsNew(std::string_view record, std::string_view current) {
  const std::optional<std::string> recorded = RecordedVersion(record);
  if (!recorded) {
    return false;
  }
  const std::optional<Version> previous = ParseVersion(*recorded);
  const std::optional<Version> now = ParseVersion(current);
  return previous && now && CompareVersions(*previous, *now) < 0;
}

HandoffOutcome CheckHandoff(std::string_view record, std::string_view current) {
  const std::optional<std::string> recorded = RecordedVersion(record);
  if (!recorded) {
    return HandoffOutcome::kNone;
  }
  const std::optional<Version> handed = ParseVersion(*recorded);
  const std::optional<Version> now = ParseVersion(current);
  if (!handed || !now) {
    return HandoffOutcome::kNone;
  }
  const int order = CompareVersions(*handed, *now);
  return order == 0 ? HandoffOutcome::kInstalled
                    : order > 0 ? HandoffOutcome::kNotInstalled : HandoffOutcome::kNone;
}

std::string NormalizeNotes(std::string_view raw) {
  if (raw.size() > kMaxBundledNotesBytes || !utf8::IsValid(raw)) {
    return std::string();
  }
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') {
        continue;
      }
      out.push_back('\n');
      continue;
    }
    out.push_back(raw[i]);
  }
  return out;
}

}  // namespace torfox::update
