// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_CMDLINE_POLICY_H_
#define TORFOX_CORE_CMDLINE_POLICY_H_

#include <map>
#include <string>
#include <vector>

namespace torfox::cmdline {

// Switch name (as Chromium's parser normalises it, without leading dashes) ->
// value ("" if none). The browser layer fills these with CefCommandLine, so
// parsing matches Chromium's exactly (including "/switch" on Windows).
using SwitchMap = std::map<std::string, std::string>;

struct Input {
  // What the user, a shortcut or another program passed to TorFox, parsed
  // from the raw OS command line.
  SwitchMap user_switches;
  std::vector<std::string> user_args;
  // The browser-process command line as CEF hands it to
  // OnBeforeCommandLineProcessing (user switches plus CEF's own).
  SwitchMap current_switches;
  // Builds with TORFOX_TEST_HOOKS accept a fixed set of test switches.
  bool test_hooks = false;
};

struct Plan {
  SwitchMap switches;                 // Final switches.
  std::vector<std::string> args;      // Final arguments (http/https URLs only).
  std::vector<std::string> removed;   // User switches that were dropped.
  std::vector<std::string> dropped_args;
};

// Applies TorFox's command-line policy:
//  * User switches are dropped unless allowlisted. Release builds allowlist
//    nothing, so a shortcut cannot pass --no-sandbox, --proxy-server,
//    --remote-debugging-port, --disable-web-security, --user-data-dir, etc.
//  * Switches CEF added itself are kept. For --enable-features and
//    --disable-features, the user's entries are removed and CEF's are kept.
//  * TorFox's hardening switches are always set (ForcedSwitches()), and
//    TorFox's feature lists are merged into CEF's.
//  * Arguments are kept only if they are absolute http or https URLs.
Plan Apply(const Input& input);

// Switch names that test-hook builds accept from the user.
const std::vector<std::string>& TestHookSwitches();

// Switches TorFox always sets, with their values. |test_host_rules| (test
// builds only) is appended after TorFox's own resolver rules, so it can
// never override the .onion rule.
SwitchMap ForcedSwitches(const std::string& test_host_rules);

const std::vector<std::string>& ForcedEnableFeatures();
const std::vector<std::string>& ForcedDisableFeatures();

// Splits "a,b,c", trimming spaces and dropping empty entries.
std::vector<std::string> SplitFeatureList(const std::string& value);
std::string JoinFeatureList(const std::vector<std::string>& features);

bool IsAbsoluteHttpUrl(const std::string& arg);

}  // namespace torfox::cmdline

#endif  // TORFOX_CORE_CMDLINE_POLICY_H_
