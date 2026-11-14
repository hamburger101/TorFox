// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_FAULT_INJECTION_H_
#define TORFOX_BROWSER_APP_FAULT_INJECTION_H_

#include <string_view>

namespace torfox::fault_injection {

// Test builds only (TORFOX_TEST_HOOKS). TorFox's defences are layered, and an
// outer layer hides the ones behind it from end-to-end tests. These switches
// let a test turn one outer layer off, so the layers behind it are verified
// on their own:
//   --torfox-test-skip-guards=nav            OnBeforeBrowse allows everything
//   --torfox-test-skip-guards=nav,resource   ... and so does OnBeforeResourceLoad
// In release builds Configure() does nothing and every query is false; the
// command-line policy also drops the switch.
void Configure(std::string_view spec);
bool SkipNavigationGuard();
bool SkipResourceGuard();

}  // namespace torfox::fault_injection

#endif  // TORFOX_BROWSER_APP_FAULT_INJECTION_H_
