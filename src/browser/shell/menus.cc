// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/menus.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "src/browser/app/app_common.h"
#include "src/browser/app/test_log.h"
#include "src/browser/platform/platform.h"

namespace torfox::menus {

namespace {

std::string g_choice_file;

struct Item {
  std::string path;
  int command_id;
  bool enabled;
  bool checked;
};

void Collect(CefRefPtr<CefMenuModel> model, const std::string& prefix, std::vector<Item>* out,
             int depth) {
  if (!model || depth > 8) {
    return;
  }
  for (size_t i = 0; i < model->GetCount(); ++i) {
    const cef_menu_item_type_t type = model->GetTypeAt(i);
    if (type == MENUITEMTYPE_SEPARATOR) {
      continue;
    }
    const std::string label = model->GetLabelAt(i).ToString();
    const std::string path = prefix.empty() ? label : prefix + "/" + label;
    if (type == MENUITEMTYPE_SUBMENU) {
      Collect(model->GetSubMenuAt(i), path, out, depth + 1);
      continue;
    }
    out->push_back(Item{path, model->GetCommandIdAt(i), model->IsEnabledAt(i),
                        model->IsCheckedAt(i)});
  }
}

}  // namespace

void ConfigureTestChoice(const std::string& path) {
  if (kTestHooks) {
    g_choice_file = path;
  }
}

std::optional<int> TestChoice(CefRefPtr<CefMenuModel> model, const char* kind) {
  if (!kTestHooks || g_choice_file.empty()) {
    return std::nullopt;
  }
  std::vector<Item> items;
  Collect(model, "", &items, 0);

  const std::filesystem::path file = platform::PathFromUtf8(g_choice_file);
  std::string choice;
  bool have_choice = false;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      std::stringstream ss;
      ss << in.rdbuf();
      choice = ss.str();
      while (!choice.empty() && (choice.back() == '\n' || choice.back() == '\r')) {
        choice.pop_back();
      }
      have_choice = true;
    }
  }
  std::error_code ec;
  std::filesystem::remove(file, ec);  // One choice per menu.

  int picked = -1;
  Json logged = Json::array();
  for (const Item& item : items) {
    logged.push_back({{"label", item.path},
                      {"id", item.command_id},
                      {"enabled", item.enabled},
                      {"checked", item.checked}});
    if (have_choice && item.enabled && item.path == choice && picked < 0) {
      picked = item.command_id;
    }
  }
  test_log::Event("menu_shown",
                  {{"kind", kind}, {"items", logged}, {"choice", choice}, {"picked", picked}});
  return picked;
}

void Show(CefRefPtr<CefWindow> window,
          CefRefPtr<CefMenuModel> model,
          CefRefPtr<Delegate> delegate,
          const CefPoint& screen_point,
          cef_menu_anchor_position_t anchor,
          const char* kind) {
  if (!window || !model) {
    return;
  }
  if (auto choice = TestChoice(model, kind)) {
    if (*choice >= 0) {
      delegate->ExecuteCommand(model, *choice, EVENTFLAG_NONE);
    }
    return;
  }
  window->ShowMenu(model, screen_point, anchor);
}

}  // namespace torfox::menus
