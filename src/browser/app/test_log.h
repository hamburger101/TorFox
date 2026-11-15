// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_TEST_LOG_H_
#define TORFOX_BROWSER_APP_TEST_LOG_H_

#include <string>
#include <string_view>

#include "src/core/json.h"

namespace torfox {

// Structured event log for automated tests: one JSON object per line, written
// to the file named by --torfox-test-log. Compiled in only when
// TORFOX_TEST_HOOKS is set; in release builds every call is a no-op and the
// switch is rejected by the command-line policy.
namespace test_log {

void Open(const std::string& path);
void Close();
bool Enabled();
// Thread-safe.
void Event(std::string_view name, Json fields = Json::object());
// Test builds: on abort() or a pure virtual call, write the call stack to the
// log ("crash_trace") before the process dies.
void InstallCrashTrace();

}  // namespace test_log

#if defined(TORFOX_TEST_HOOKS)
inline constexpr bool kTestHooks = true;
#else
inline constexpr bool kTestHooks = false;
#endif

}  // namespace torfox

#endif  // TORFOX_BROWSER_APP_TEST_LOG_H_
