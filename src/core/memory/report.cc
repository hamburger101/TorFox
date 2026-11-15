// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/memory/report.h"

#include <algorithm>
#include <map>
#include <set>

#include "src/core/util/utf8.h"

namespace torfox::memory {

namespace {

struct Group {
  uint64_t bytes = 0;
  std::vector<long> pids;

  void Add(const Process& p) {
    bytes += p.bytes;
    pids.push_back(p.pid);
  }
};

Json GroupJson(const Group& g) {
  return {{"bytes", g.bytes}, {"processes", static_cast<int>(g.pids.size())}, {"pids", g.pids}};
}

// At most |kMaxTitleBytes|, cut on a character boundary.
std::string ShortTitle(const std::string& title) {
  if (title.size() <= kMaxTitleBytes) {
    return title;
  }
  std::size_t end = kMaxTitleBytes;
  while (end > 0 && (static_cast<unsigned char>(title[end]) & 0xC0) == 0x80) {
    --end;
  }
  return title.substr(0, end) + "\xE2\x80\xA6";  // An ellipsis.
}

}  // namespace

Json BuildReport(const std::vector<Process>& processes, const std::vector<Renderer>& renderers) {
  // What each helper process hosts, by what its renderers said.
  std::map<long, std::set<int64_t>> tabs_in;
  std::set<long> interface_in;
  std::map<int64_t, const Renderer*> tab_info;
  for (const Renderer& r : renderers) {
    if (r.pid <= 0) {
      continue;
    }
    if (r.tab) {
      tabs_in[r.pid].insert(r.tab_key);
      tab_info.emplace(r.tab_key, &r);
    } else {
      interface_in.insert(r.pid);
    }
  }

  uint64_t total = 0;
  uint64_t browser = 0;
  Group interface_group;
  Group helpers;
  Group tor;
  std::map<std::set<int64_t>, Group> rows;
  std::set<long> seen;
  for (const Process& p : processes) {
    if (p.pid <= 0 || !seen.insert(p.pid).second) {
      continue;  // Each process counts once.
    }
    total += p.bytes;
    switch (p.kind) {
      case Process::Kind::kBrowser:
        browser += p.bytes;
        continue;
      case Process::Kind::kTor:
        tor.Add(p);
        continue;
      case Process::Kind::kHelper:
        break;
    }
    if (auto it = tabs_in.find(p.pid); it != tabs_in.end()) {
      rows[it->second].Add(p);
    } else if (interface_in.count(p.pid) != 0) {
      interface_group.Add(p);
    } else {
      helpers.Add(p);
    }
  }

  std::vector<std::pair<const std::set<int64_t>*, Group>> sorted;
  for (const auto& [keys, group] : rows) {
    sorted.emplace_back(&keys, group);
  }
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
  Json tab_rows = Json::array();
  std::size_t listed = 0;
  for (const auto& [keys, group] : sorted) {
    Json tabs = Json::array();
    for (int64_t key : *keys) {
      if (listed >= kMaxTabsListed) {
        break;
      }
      const Renderer* r = tab_info[key];
      const bool normal = r->window == "normal";
      const std::string window = normal || r->window == "tor" ? r->window : "private";
      // Private and Tor tabs are not named here: Settings can be open in
      // any window.
      tabs.push_back({{"title", normal && utf8::IsValid(r->title) ? ShortTitle(r->title) : ""},
                      {"window", window},
                      {"active", r->active}});
      ++listed;
    }
    if (tabs.empty()) {
      continue;
    }
    Json row = GroupJson(group);
    row["tabs"] = std::move(tabs);
    tab_rows.push_back(std::move(row));
  }

  return {{"supported", !processes.empty()},
          {"total", total},
          {"processes", static_cast<int>(seen.size())},
          {"browser", browser},
          {"interface", GroupJson(interface_group)},
          {"helpers", GroupJson(helpers)},
          {"tor", GroupJson(tor)},
          {"tabs", std::move(tab_rows)}};
}

}  // namespace torfox::memory
