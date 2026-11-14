// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/fault_injection.h"

#if defined(TORFOX_TEST_HOOKS)

#include <atomic>
#include <string>

#include "src/browser/app/test_log.h"

namespace torfox::fault_injection {

namespace {

std::atomic<bool> g_skip_nav{false};
std::atomic<bool> g_skip_resource{false};

}  // namespace

void Configure(std::string_view spec) {
  Json applied = Json::array();
  std::size_t start = 0;
  while (start <= spec.size()) {
    std::size_t end = spec.find(',', start);
    if (end == std::string_view::npos) {
      end = spec.size();
    }
    const std::string_view item = spec.substr(start, end - start);
    if (item == "nav") {
      g_skip_nav = true;
      applied.push_back("nav");
    } else if (item == "resource") {
      g_skip_resource = true;
      applied.push_back("resource");
    }
    start = end + 1;
  }
  if (!applied.empty()) {
    test_log::Event("fault_injection", {{"skip_guards", applied}});
  }
}

bool SkipNavigationGuard() {
  return g_skip_nav;
}

bool SkipResourceGuard() {
  return g_skip_resource;
}

}  // namespace torfox::fault_injection

#else  // !TORFOX_TEST_HOOKS

namespace torfox::fault_injection {

void Configure(std::string_view) {}

bool SkipNavigationGuard() {
  return false;
}

bool SkipResourceGuard() {
  return false;
}

}  // namespace torfox::fault_injection

#endif  // TORFOX_TEST_HOOKS
