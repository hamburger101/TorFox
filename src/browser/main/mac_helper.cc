// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// macOS sub-process entry point, shared by every helper bundle
// ("TorFox Helper", "(Renderer)", "(GPU)", "(Plugin)", "(Alerts)").

#include <cstring>

#include "include/cef_app.h"
#include "include/cef_sandbox_mac.h"
#include "include/wrapper/cef_library_loader.h"
#include "src/browser/app/browser_app.h"

#if !defined(CEF_USE_SANDBOX)
#error "TorFox helpers must be built with the sandbox (USE_SANDBOX=ON)."
#endif

int main(int argc, char* argv[]) {
  // Helpers exist only to run Chromium sub-processes, which always carry a
  // --type switch. Anything else is not a legitimate launch.
  bool has_type = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--type=", 7) == 0) {
      has_type = true;
    }
  }
  if (!has_type) {
    return 1;
  }

  // Enter the macOS sandbox before any other code runs in this process.
  CefScopedSandboxContext sandbox_context;
  if (!sandbox_context.Initialize(argc, argv)) {
    return 1;
  }

  // Load the CEF framework at run time, as the sandbox requires.
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInHelper()) {
    return 1;
  }

  CefMainArgs main_args(argc, argv);
  return torfox::TorFoxMain(main_args, nullptr);
}
