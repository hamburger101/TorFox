// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/browser_registry.h"

namespace torfox {

namespace {
// Namespace-scope (not function-local) so construction happens at load time,
// before any thread exists; CEF builds on macOS disable thread-safe statics.
BrowserRegistry g_registry;
}  // namespace

BrowserRegistry& Registry() {
  return g_registry;
}

std::string_view RoleName(BrowserRole role) {
  switch (role) {
    case BrowserRole::kShell:
      return "shell";
    case BrowserRole::kInternalTab:
      return "internal_tab";
    case BrowserRole::kWebTab:
      return "web_tab";
  }
  return "unknown";
}

bool IsInternalRole(BrowserRole role) {
  return guards::IsInternal(GuardRole(role));
}

guards::Role GuardRole(BrowserRole role) {
  switch (role) {
    case BrowserRole::kShell:
      return guards::Role::kShell;
    case BrowserRole::kInternalTab:
      return guards::Role::kInternalTab;
    case BrowserRole::kWebTab:
      return guards::Role::kWebTab;
  }
  return guards::Role::kWebTab;  // Unreachable; the least privileged role.
}

void BrowserRegistry::Add(int browser_id, const BrowserEntry& entry) {
  std::lock_guard<std::mutex> guard(lock_);
  entries_[browser_id] = entry;
}

void BrowserRegistry::Remove(int browser_id) {
  std::lock_guard<std::mutex> guard(lock_);
  entries_.erase(browser_id);
}

std::optional<BrowserEntry> BrowserRegistry::Find(int browser_id) const {
  std::lock_guard<std::mutex> guard(lock_);
  auto it = entries_.find(browser_id);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool BrowserRegistry::IsInternal(int browser_id) const {
  auto entry = Find(browser_id);
  return entry && IsInternalRole(entry->role);
}

std::size_t BrowserRegistry::size() const {
  std::lock_guard<std::mutex> guard(lock_);
  return entries_.size();
}

}  // namespace torfox
