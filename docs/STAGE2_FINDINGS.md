# Stage 2 findings (2026-11-14)

What Stage 2 (browsing) established, what it found, and what it leaves open. Every claim here comes from a
test in this repository; [`TESTING.md`](TESTING.md) says how to run them. All results were measured on
Windows 11 x64 with CEF 156.0.30 (Chromium 156.0.8078.96), using Release builds. macOS is built by CI
only, and none of the Stage 2 behaviour has been run on macOS.

## Results at a glance
| Suite | Result |
|---|---|
| Unit tests (`torfox_unit_tests`) | 93 passed |
| End-to-end tests (test build) | 121 tests: 119 passed, plus 2 expected failures for known gaps (WPAD, Stage 1 §4; zoom levels, §5 below) |
| Mutation check | All 24 mutations detected (20 from Stage 1, 4 new) |

## 1. What was built
- **Tabs:** new, close, select, reorder (drag in the strip, Ctrl+Shift+PgUp/PgDn, `tab.move`), reopen closed
  (Ctrl+Shift+T; up to 25 per window), links opened from a page are placed next to their opener.
- **Address bar:** the Stage 1 classifier with Public Suffix List host detection. Search goes to
  DuckDuckGo; `.onion` input is refused (no Tor yet) and is never searched.
- **Navigation:** back, forward, reload (and reload bypassing cache), stop, home.
- **Find in page:** a find bar that pushes the page down (it never covers content), match count, next and
  previous, match case.
- **Zoom:** Ctrl+= / Ctrl+- / Ctrl+0, with a zoom badge in the address bar.
- **Bookmarks:** star button, bookmarks bar (off by default; Ctrl+Shift+B or the app menu), and a
  `torfox://bookmarks/` manager with folders, search, edit, move and delete.
- **History:** `torfox://history/` with day groups, search, paging, remove one entry, and clear all with
  a confirmation step.
- **Private windows:** Ctrl+Shift+N, the app menu, or "Open link in private window".
- **Keyboard shortcuts:** one table (`src/core/browsing/shortcuts.cc`) with separate Windows and macOS
  bindings, unit-tested for both platforms.
- **Menus:** TorFox's own native menus (CEF Views): the app menu, the star menu, bookmark folders, and
  the page context menu.
- **Error pages:** Chromium's network error pages; the address bar keeps the URL that failed.

## 2. Chromium's own history is switched off, and TorFox's is the only record
Chromium's history service records every visit in `<profile>/History`. TorFox's History page could
neither show nor clear that database, so TorFox sets `history.saving_disabled` on every request context and
keeps its own record in `places.db` (SQLite, `secure_delete` on, WAL checkpointed after every removal).

- `test_visits_are_recorded_by_torfox_not_chromium` checks that a visit appears in TorFox's history, that
  Chromium's `History` database has no rows, and that no file in Chromium's profile holds the page's path.
- The `chromium-history-enabled` mutation removes the pref and is detected, so **the pref is necessary**: without
  it, Alloy-style browsers do record visits in Chromium's database.
- Removing an entry or clearing history overwrites the data in `places.db` (`secure_delete`) and truncates the
  WAL. `test_history_page_search_remove_and_clear` checks that the removed URLs no longer appear in any file in
  the profile. On SSDs and copy-on-write file systems the old blocks may survive below the file system; TorFox
  can't do anything about that, and says so on the History page only as "overwritten in TorFox's files".
- Only successful page loads in Normal windows are recorded. Error pages (no HTTP response, or the URL that just
  failed) and TorFox's own pages are not (`test_internal_pages_and_failed_loads_are_not_recorded`).

## 3. Private windows
Each private window gets its own off-the-record request context (`CreateContext` with an empty cache path,
which gives a unique OTR profile). Its prefs are applied and read back before the window opens; if anything fails,
no window opens and the requesting window shows the error.

| Test | Shows |
|---|---|
| `test_private_window_is_marked_and_records_nothing` | The window, shell and title are marked private; a visit is not recorded |
| `test_storage_is_isolated_between_windows` | Cookies and localStorage for one origin differ between a normal window and two private windows |
| `test_private_tabs_and_links_stay_private` | New tabs and opened links stay in the window's own private context |
| `test_open_link_in_private_window_from_a_normal_window` | The link opens in a new private context without the normal window's cookies |
| `test_nothing_from_a_private_window_reaches_the_disk` | After cookies, localStorage and IndexedDB were written in a private window and TorFox exited, no file in the profile holds the page's unique host name (searched as UTF-8 and UTF-16) |

The last test has a control: the same page in a normal window must leave a trace that the same search finds,
so the search is known to work. Both private-window mutations are detected: recording private history,
and giving private windows the Normal context.

Deliberate choices:
- Bookmarking from a private window is allowed. It is an explicit user action, and the bookmark goes into
  the normal bookmarks (like other browsers).
- Each private window is separate: two private windows don't share cookies.

## 4. Find selected no match on the first search (fixed)
CEF's `Find(text, forward, matchCase, findNext)` with `findNext=false` highlights every match but makes none
of them active, so the first Enter skipped to match 2. CEF restarts a search by itself when the text or case
changes, so TorFox now always passes `findNext=true`. `test_find_in_page` caught this.

## 5. Zoom levels outlive cleared history (known gap)
Chromium stores zoom per host in the Normal profile's `Preferences` file (`partition.per_host_zoom_levels`).
Clearing history doesn't touch it, so a zoomed site's host name stays on disk. Private windows keep zoom in
memory only. `test_clearing_history_forgets_zoomed_sites` is an expected failure until the Stage 4
clear-data work clears zoom levels too.

## 6. Pop-ups and links
Chromium's own pop-up blocker refuses `window.open` without a user gesture before TorFox's `OnBeforePopup`
runs (already recorded in Stage 1). A click on a `target=_blank` link opens a tab next to its opener, with no
opener relationship.

## 7. Context menus
TorFox builds the page menu from scratch: no Save, Print, View source, Inspect or Download items. The search item
("Search DuckDuckGo for “…”") is left out for any selection containing `.onion`, in any letter case. This is
stricter than the address bar (unit test `ContextMenu.NeverSearchesOnionAddresses`, E2E
`test_onion_selections_are_never_searched`, and the `onion-selections-searchable` mutation).

## 8. Background traffic seen during testing
In one full run, someone's keystrokes reached a TorFox test window, which then searched DuckDuckGo for real.
The quiet-startup NetLog of that run shows that loading `duckduckgo.com` also made Chromium contact
`content-autofill.googleapis.com`: Chromium's autofill server queries. The idle test passes when nothing is
typed, but this is a real leak for normal browsing. Disabling autofill server communication is Stage 4 work.

## 9. Left for later stages
| Item | Stage |
|---|---|
| Clearing zoom levels with history (§5) | 4 |
| Autofill server queries (§8), autofill and password-manager prefs | 4 |
| Address-bar suggestions from history and bookmarks (`Places::Suggest` exists and is unit-tested; the shell doesn't show suggestions yet) | 6 |
| A tab context menu (close others, duplicate) | 6 |
| macOS main menu (NSMenu) entries for the Stage 2 commands; macOS keyboard shortcuts are mapped and unit-tested | 6 |
| Reopened tabs get their URL back, not their back/forward history | not planned |

## 10. Limits of this evidence
- Everything above was measured on Windows. macOS gets the same code, but none of it has been run there.
- Keyboard shortcuts are exercised with DevTools key events, which go through the same `OnPreKeyEvent` path
  as the keyboard. Native menus are exercised through a test-build hook (`--torfox-test-menu-choice`), which
  picks an item instead of showing the menu. Painting of native menus is not tested.
- The no-trace test searches every file in TorFox's profile folder. Files outside it (for example the system
  temp folder) are not searched.
