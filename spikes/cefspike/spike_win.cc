// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <windows.h>

#include "include/cef_sandbox_win.h"
#include "include/cef_version_info.h"
#include "spikes/cefspike/spike.h"

#if !defined(CEF_USE_BOOTSTRAP)
#error "The spike must be built as a bootstrap DLL (USE_SANDBOX=ON)."
#endif

// Entry point called by bootstrap.exe (renamed to cefspike.exe).
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance,
                                    LPWSTR /*lpCmdLine*/,
                                    int /*nCmdShow*/,
                                    void* sandbox_info,
                                    cef_version_info_t* /*version_info*/) {
  CefMainArgs main_args(hInstance);
  return SpikeMain(main_args, sandbox_info);
}
