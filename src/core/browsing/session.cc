// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/session.h"

#include "src/core/browsing/places.h"
#include "src/core/json.h"
#include "src/core/url/guards.h"
#include "src/core/url/internal_url.h"

namespace torfox::browsing {

std::optional<std::string> RestorableUrl(std::string_view url) {
  if (HasInternalScheme(url)) {
    const std::optional<InternalUrl> parsed = ParseInternalUrl(url);
    if (!parsed || !IsInternalPage(parsed->host) ||
        !guards::PageAllowedForRole(parsed->host, guards::Role::kInternalTab)) {
      return std::nullopt;
    }
    return InternalPageUrl(parsed->host);
  }
  if (!IsStorableUrl(url) || guards::IsOnionHost(guards::HostOf(url))) {
    return std::nullopt;
  }
  return std::string(url);
}

std::string SerializeSession(const Session& session) {
  Json windows = Json::array();
  for (const SessionWindow& w : session) {
    if (windows.size() >= kMaxSessionWindows) {
      break;
    }
    Json tabs = Json::array();
    std::size_t active = 0;
    for (std::size_t i = 0; i < w.urls.size() && tabs.size() < kMaxSessionTabs; ++i) {
      if (std::optional<std::string> url = RestorableUrl(w.urls[i])) {
        if (i <= w.active) {
          active = tabs.size();  // The kept tab at or before the active one.
        }
        tabs.push_back(*url);
      }
    }
    if (!tabs.empty()) {
      windows.push_back(Json{{"tabs", std::move(tabs)}, {"active", active}});
    }
  }
  return DumpJson(Json{{"format", 1}, {"windows", std::move(windows)}});
}

Session ParseSession(std::string_view text) {
  Session session;
  std::optional<Json> root = ParseJson(text, kMaxSessionBytes, 8);
  if (!root || !root->is_object()) {
    return session;
  }
  auto format = root->find("format");
  auto windows = root->find("windows");
  if (format == root->end() || *format != Json(1) || windows == root->end() ||
      !windows->is_array()) {
    return session;
  }
  for (const Json& w : *windows) {
    if (session.size() >= kMaxSessionWindows) {
      break;
    }
    if (!w.is_object()) {
      continue;
    }
    auto tabs = w.find("tabs");
    if (tabs == w.end() || !tabs->is_array()) {
      continue;
    }
    SessionWindow window;
    for (const Json& t : *tabs) {
      if (window.urls.size() >= kMaxSessionTabs) {
        break;
      }
      if (!t.is_string()) {
        continue;
      }
      if (std::optional<std::string> url = RestorableUrl(t.get_ref<const std::string&>())) {
        window.urls.push_back(std::move(*url));
      }
    }
    if (window.urls.empty()) {
      continue;
    }
    auto active = w.find("active");
    if (active != w.end() && active->is_number_unsigned()) {
      const auto a = active->get<std::size_t>();
      window.active = a < window.urls.size() ? a : window.urls.size() - 1;
    }
    session.push_back(std::move(window));
  }
  return session;
}

}  // namespace torfox::browsing
