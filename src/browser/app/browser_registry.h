// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_BROWSER_REGISTRY_H_
#define TORFOX_BROWSER_APP_BROWSER_REGISTRY_H_

#include <map>
#include <mutex>
#include <optional>
#include <string_view>

#include "src/core/url/guards.h"

namespace torfox {

class BrowserWindow;

// What a browser is for. Fixed when the browser is created; a tab that
// changes kind gets a new browser.
enum class BrowserRole {
  kShell,        // The window's tab strip/toolbar (torfox://shell/).
  kInternalTab,  // A tab showing an internal page (torfox://newtab/, ...).
  kWebTab,       // A tab showing web content (Normal context).
};

std::string_view RoleName(BrowserRole role);
bool IsInternalRole(BrowserRole role);
// The core guards' view of a role (see src/core/url/guards.h).
guards::Role GuardRole(BrowserRole role);

struct BrowserEntry {
  BrowserRole role;
  BrowserWindow* window;  // UI thread only.
  int tab_id;             // 0 for the shell.
};

// Browser-process record of every live browser. This, not anything a
// renderer says, decides whether a browser is internal. Thread-safe: the
// scheme handler factory consults it on the IO thread.
class BrowserRegistry {
 public:
  void Add(int browser_id, const BrowserEntry& entry);
  void Remove(int browser_id);
  std::optional<BrowserEntry> Find(int browser_id) const;
  bool IsInternal(int browser_id) const;
  std::size_t size() const;

 private:
  mutable std::mutex lock_;
  std::map<int, BrowserEntry> entries_;
};

// The single registry owned by the browser process.
BrowserRegistry& Registry();

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_BROWSER_REGISTRY_H_
