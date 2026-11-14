// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_DIALOGS_H_
#define TORFOX_BROWSER_SHELL_DIALOGS_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/views/cef_window.h"
#include "src/browser/platform/platform.h"

// Native dialogs, with a scripted responder in test builds.
namespace torfox::dialogs {

// Test builds (--torfox-test-dialog-choice=<file>): instead of showing a
// dialog, log it ("dialog_shown") and answer from the file, which is deleted
// each time. Confirmations: "accept", "cancel", or "hold" (no answer until the
// prompt is cancelled by TorFox). File dialogs: "cancel" or "file:<path>". A
// missing file or anything else means cancel.
void ConfigureTestChoice(const std::string& path);

// Shows a native yes/no prompt (platform::ShowConfirmPrompt). |done| runs
// exactly once on the UI thread; it gets false if the prompt is cancelled
// through the returned handle, or if no dialog could be shown. Must be called
// on the UI thread.
std::shared_ptr<platform::PromptHandle> Confirm(CefRefPtr<CefWindow> window,
                                                const char* kind,
                                                const platform::ConfirmPrompt& prompt,
                                                std::function<void(bool)> done);

// Test builds with the hook configured: the scripted answer to a file
// dialog (empty = cancel). nullopt when the hook is off, so the real dialog
// runs.
std::optional<std::vector<std::string>> TestFileChoice(const char* kind, int mode);

}  // namespace torfox::dialogs

#endif  // TORFOX_BROWSER_SHELL_DIALOGS_H_
