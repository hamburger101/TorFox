// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/cmdline/policy.h"

#include <algorithm>

namespace torfox::cmdline {

namespace {

const std::vector<std::string> kTestHookSwitches = {
    "remote-debugging-port",  // Chromium binds it to loopback only.
    "log-net-log",
    "net-log-capture-mode",
    "enable-logging",
    "v",
    "log-file",
    "log-level",
    "torfox-test-log",
    "torfox-test-user-data",
    "torfox-test-host-rules",
    "torfox-test-start-url",
    "torfox-test-skip-guards",
    "torfox-test-menu-choice",  // File naming the menu item to pick (tests/e2e).
    "torfox-test-dialog-choice",  // File naming the answer to the next dialog.
    "torfox-test-download-dir",   // Folder used instead of Downloads.
    "torfox-test-https-port",     // <http>:<https> port pair HTTPS-First may map.
    "torfox-test-tor-command",    // JSON file: {"exe", "args"} run instead of tor.
    // The privacy tests' local https sites: their certificate is trusted by
    // its public key, and a fake camera and microphone answer getUserMedia.
    "ignore-certificate-errors-spki-list",
    "use-fake-device-for-media-stream",
    // Other windows may cover the test windows. Chromium then treats them as
    // hidden: no frames, so input waits (Stage 7 findings, §3.5).
    "disable-backgrounding-occluded-windows",
    "torfox-test-update-config",  // JSON file: a fake GitHub and its key.
};

// Evidence for each entry is in docs/STAGE0_FINDINGS.md and the quiet-startup
// test (tests/e2e/test_quiet_startup.py).
const std::vector<std::string> kEnableFeatures = {
    "NetworkServiceSandbox",  // Run the network service in an AppContainer.
};
const std::vector<std::string> kDisableFeatures = {
    "HttpsUpgrades",  // Replaced by TorFox's own HTTPS-first policy.
    // Background services that contact Google while TorFox sits idle. The
    // Chrome runtime starts them even with --disable-background-networking
    // and --disable-component-update; found by the quiet-startup test.
    // Each entry is needed on its own: tests/e2e/run_mutations.py removes
    // them one at a time and the quiet-startup test then fails.
    "NetworkTimeServiceQuerying",        // clients2.google.com/time/1/current
    "TranslateRankerQuery",              // www.gstatic.com: translate ranker model
    "TranslateRankerEnforcement",        //   (loaded if either is enabled)
    "AimServerRequestOnStartupEnabled",  // www.google.com/async/folae: AI mode eligibility
};

// Both spellings: "x.onion." (a fully qualified name) is the same host to
// DNS but does not match "*.onion" (docs/STAGE9_FINDINGS.md).
constexpr char kOnionRule[] = "MAP *.onion ~NOTFOUND,MAP *.onion. ~NOTFOUND";

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

}  // namespace

const std::vector<std::string>& TestHookSwitches() {
  return kTestHookSwitches;
}

const std::vector<std::string>& ForcedEnableFeatures() {
  return kEnableFeatures;
}

const std::vector<std::string>& ForcedDisableFeatures() {
  return kDisableFeatures;
}

SwitchMap ForcedSwitches(const std::string& test_host_rules) {
  SwitchMap s = {
      {"site-per-process", ""},
      {"disable-extensions", ""},
      {"disable-background-networking", ""},
      {"disable-component-update", ""},
      // Some components are registered even with --disable-component-update
      // (the on-device model manifest, seen by the quiet-startup test). Point
      // the component updater at a port Chromium refuses to connect to, so no
      // component can ever check for updates, download, or ping.
      {"component-updater", "url-source=http://127.0.0.1:9/,disable-pings"},
      {"disable-domain-reliability", ""},
      {"disable-client-side-phishing-detection", ""},
      {"disable-default-apps", ""},
      {"disable-sync", ""},
      {"disable-search-engine-choice-screen", ""},
      {"no-pings", ""},
      {"no-first-run", ""},
      {"no-default-browser-check", ""},
      // The File System Access API's local pickers (showSaveFilePicker,
      // showOpenFilePicker, showDirectoryPicker) let pages write files
      // outside the download path, and in Alloy-style browsers a save into a
      // folder Chromium considers sensitive hangs and closing the tab then
      // crashes the browser (docs/STAGE3_FINDINGS.md). Firefox, Safari and
      // Tor Browser do not offer them either. <input type=file> is unaffected.
      {"disable-blink-features", "FileSystemAccessLocal"},
  };
  std::string rules = kOnionRule;
  if (!test_host_rules.empty()) {
    rules += "," + test_host_rules;
  }
  s["host-resolver-rules"] = rules;
  return s;
}

std::vector<std::string> SplitFeatureList(const std::string& value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    std::size_t end = value.find(',', start);
    if (end == std::string::npos) {
      end = value.size();
    }
    std::string item = value.substr(start, end - start);
    std::size_t a = item.find_first_not_of(' ');
    std::size_t b = item.find_last_not_of(' ');
    if (a != std::string::npos) {
      out.push_back(item.substr(a, b - a + 1));
    }
    start = end + 1;
  }
  return out;
}

std::string JoinFeatureList(const std::vector<std::string>& features) {
  std::string out;
  for (const std::string& f : features) {
    if (!out.empty()) {
      out += ",";
    }
    out += f;
  }
  return out;
}

bool IsAbsoluteHttpUrl(const std::string& arg) {
  if (arg.size() > 8192) {
    return false;
  }
  std::string lower = LowerAscii(arg.substr(0, 8));
  std::size_t prefix = 0;
  if (lower.rfind("https://", 0) == 0) {
    prefix = 8;
  } else if (lower.rfind("http://", 0) == 0) {
    prefix = 7;
  } else {
    return false;
  }
  if (arg.size() == prefix) {
    return false;
  }
  for (char c : arg) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7F) {
      return false;
    }
  }
  return true;
}

Plan Apply(const Input& input) {
  Plan plan;
  plan.switches = input.current_switches;

  // 1. Drop user switches that are not allowlisted.
  for (const auto& [name, value] : input.user_switches) {
    if (input.test_hooks && Contains(kTestHookSwitches, name)) {
      continue;
    }
    plan.removed.push_back(name);
    if (name == "enable-features" || name == "disable-features") {
      auto it = plan.switches.find(name);
      if (it == plan.switches.end()) {
        continue;
      }
      std::vector<std::string> user = SplitFeatureList(value);
      std::vector<std::string> kept;
      for (const std::string& f : SplitFeatureList(it->second)) {
        if (!Contains(user, f)) {
          kept.push_back(f);
        }
      }
      if (kept.empty()) {
        plan.switches.erase(it);
      } else {
        it->second = JoinFeatureList(kept);
      }
      continue;
    }
    // Only the user's own switch goes. A switch of the same name with
    // another value is CEF's (release builds never even pass the user's
    // switches to Chromium: command_line_args_disabled), and erasing it would
    // undo a CEF setting such as log-severity=disable.
    auto it = plan.switches.find(name);
    if (it != plan.switches.end() && it->second == value) {
      plan.switches.erase(it);
    }
  }

  // 2. Forced switches always win.
  std::string test_rules;
  if (input.test_hooks) {
    auto it = input.user_switches.find("torfox-test-host-rules");
    if (it != input.user_switches.end()) {
      test_rules = it->second;
    }
  }
  for (const auto& [name, value] : ForcedSwitches(test_rules)) {
    plan.switches[name] = value;
  }
  if (!input.test_hooks) {
    // No logs in release builds: they could record URLs. CEF sets this from
    // CefSettings too; forcing it here means no switch of the user's can
    // ever remove it.
    plan.switches["log-severity"] = "disable";
  }

  // 3. Merge feature lists. A feature TorFox enables can never stay disabled,
  // and vice versa.
  auto merge = [&plan](const std::string& name, const std::vector<std::string>& add,
                       const std::vector<std::string>& forbid) {
    std::vector<std::string> list;
    auto it = plan.switches.find(name);
    if (it != plan.switches.end()) {
      list = SplitFeatureList(it->second);
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&forbid](const std::string& f) { return Contains(forbid, f); }),
               list.end());
    for (const std::string& f : add) {
      if (!Contains(list, f)) {
        list.push_back(f);
      }
    }
    if (list.empty()) {
      plan.switches.erase(name);
    } else {
      plan.switches[name] = JoinFeatureList(list);
    }
  };
  merge("enable-features", kEnableFeatures, kDisableFeatures);
  merge("disable-features", kDisableFeatures, kEnableFeatures);

  // 4. Arguments: only absolute http(s) URLs.
  for (const std::string& arg : input.user_args) {
    if (IsAbsoluteHttpUrl(arg)) {
      plan.args.push_back(arg);
    } else {
      plan.dropped_args.push_back(arg);
    }
  }
  return plan;
}

}  // namespace torfox::cmdline
