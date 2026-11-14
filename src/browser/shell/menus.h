// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_SHELL_MENUS_H_
#define TORFOX_BROWSER_SHELL_MENUS_H_

#include <functional>
#include <optional>
#include <string>

#include "include/cef_menu_model.h"
#include "include/cef_menu_model_delegate.h"
#include "include/views/cef_window.h"

namespace torfox::menus {

// Runs |on_command| for the item the user picks.
class Delegate : public CefMenuModelDelegate {
 public:
  using OnCommand = std::function<void(int command_id, cef_event_flags_t flags)>;
  explicit Delegate(OnCommand on_command) : on_command_(std::move(on_command)) {}

  void ExecuteCommand(CefRefPtr<CefMenuModel> menu_model,
                      int command_id,
                      cef_event_flags_t event_flags) override {
    if (on_command_) {
      on_command_(command_id, event_flags);
    }
  }

 private:
  OnCommand on_command_;
  IMPLEMENT_REFCOUNTING(Delegate);
};

// Shows a native menu (CEF Views) at |screen_point| in |window|.
// In test builds started with --torfox-test-menu-choice, the menu is not
// shown: its items are logged and the item named in the choice file is
// picked instead (see TestChoice), so automated tests never block on a menu.
void Show(CefRefPtr<CefWindow> window,
          CefRefPtr<CefMenuModel> model,
          CefRefPtr<Delegate> delegate,
          const CefPoint& screen_point,
          cef_menu_anchor_position_t anchor,
          const char* kind);

// Test builds only. Configures the choice file (empty: hook off).
void ConfigureTestChoice(const std::string& path);

// Test builds only: if the hook is on, logs |model|'s items as a
// "menu_shown" event and returns the command id of the item whose label path
// ("Folder/Item") the choice file names, or -1 to dismiss the menu (no file,
// or no such item). The choice file is consumed. Returns nothing when the
// hook is off, and always in release builds.
std::optional<int> TestChoice(CefRefPtr<CefMenuModel> model, const char* kind);

}  // namespace torfox::menus

#endif  // TORFOX_BROWSER_SHELL_MENUS_H_
