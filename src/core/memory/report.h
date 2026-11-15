// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_MEMORY_REPORT_H_
#define TORFOX_CORE_MEMORY_REPORT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "src/core/json.h"

// Settings > Memory: how much memory TorFox's processes use, grouped the way
// the user knows them (tabs, TorFox's own pages, Chromium's helpers, Tor).
namespace torfox::memory {

// One of TorFox's processes, as the operating system reports it.
struct Process {
  enum class Kind {
    kBrowser,  // This process.
    kHelper,   // A Chromium helper started from TorFox's own files.
    kTor,      // The bundled tor or one of its pluggable transports.
  };
  long pid = 0;
  Kind kind = Kind::kHelper;
  uint64_t bytes = 0;
};

// A browser in TorFox and a process its renderer said it runs in. What a
// renderer says is only used for a process that is one of TorFox's helpers.
struct Renderer {
  long pid = 0;
  // A tab (web page or internal page) or TorFox's own interface (a
  // window's toolbar, the address bar's suggestion list).
  bool tab = false;
  int64_t tab_key = 0;  // Unique per tab while TorFox runs.
  std::string title;
  std::string window;  // "normal", "private" or "tor".
  bool active = false;  // The tab its window shows.
};

inline constexpr std::size_t kMaxTabsListed = 500;
inline constexpr std::size_t kMaxTitleBytes = 200;

// The report for memory.query:
//   {"supported", "total", "processes", "browser",
//    "interface": {"bytes", "processes", "pids"}, "helpers": {...}, "tor": {...},
//    "tabs": [{"tabs": [{"title", "window", "active"}], "bytes", "processes", "pids"}]}
// A helper process belongs to the tabs whose frames run in it (a row lists
// several tabs when they share processes); else to the interface if one of
// its pages runs in it; else to the helpers (graphics, network, spare
// renderers). Private and Tor tabs are listed without their titles. Rows
// are sorted by memory, largest first.
Json BuildReport(const std::vector<Process>& processes, const std::vector<Renderer>& renderers);

}  // namespace torfox::memory

#endif  // TORFOX_CORE_MEMORY_REPORT_H_
