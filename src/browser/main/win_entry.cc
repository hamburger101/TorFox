// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <windows.h>

#include "include/cef_sandbox_win.h"
#include "include/cef_version_info.h"
#include "src/browser/app/browser_app.h"

#if !defined(CEF_USE_BOOTSTRAP)
#error "TorFox must be built as a bootstrap DLL (USE_SANDBOX=ON) so the sandbox is available."
#endif

// Called by CEF's bootstrap.exe (shipped renamed as TorFox.exe), which owns
// sandbox initialisation for every process type.
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance,
                                    LPWSTR /*lpCmdLine*/,
                                    int /*nCmdShow*/,
                                    void* sandbox_info,
                                    cef_version_info_t* /*version_info*/) {
  CefMainArgs main_args(hInstance);
  return torfox::TorFoxMain(main_args, sandbox_info);
}
