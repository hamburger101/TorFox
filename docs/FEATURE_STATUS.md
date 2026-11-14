# Feature status

A feature counts as **Verified** only when an automated test that checks its behaviour (not just its UI)
passes on the platform listed. An interface existing is not evidence. **M** marks protections that the
mutation check also covers, where removing the protection makes its tests fail ([`TESTING.md`](TESTING.md)).

Legend:
- ✅ Verified by test
- 🟡 Implemented, not verified
- ❌ Known gap (a test records it)
- ⬜ Not started
- ⛔ Not supported (with reason)

**macOS:** the build files and platform code exist but have never been compiled, so every macOS entry is
⬜ or 🟡 until CI builds and tests them.

Last updated: end of Stage 3 (2026-11-14). The evidence is in [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md),
[`STAGE2_FINDINGS.md`](STAGE2_FINDINGS.md) and [`STAGE3_FINDINGS.md`](STAGE3_FINDINGS.md).

| Area | Feature | Windows | macOS | Evidence |
|---|---|---|---|---|
| Engine | Sandbox on for every process type, incl. network service (release build) | ✅ | 🟡 | `tests/platform/test_release_win.py`; macOS helpers initialise the sandbox, untested |
| Engine | Command-line switches can't weaken a release build | ✅ M | ⬜ | `test_command_line_cannot_weaken_torfox`, `CmdlinePolicy.*` |
| Engine | Site isolation (`--site-per-process`) | 🟡 | 🟡 | Switch forced (unit test); process separation not yet measured |
| UI isolation | `torfox://` unreachable from web content (all layers) | ✅ M | ⬜ | `test_isolation.py` (37 tests, several covering many URL variants; each guard layer on its own) |
| UI isolation | Bridge absent from web pages; every message validated | ✅ M | ⬜ | `test_isolation.py`, `test_bridge.py`, `BridgeValidator.*` |
| UI isolation | Internal pages: strict CSP, no eval or inline scripts, no frames | ✅ | ⬜ | `test_internal_pages.py` |
| UI isolation | Internal pages can't reach the web | ✅ M | ⬜ | `InternalPagesStayInternalTest` |
| Browsing | Open and close tabs and windows | ✅ | ⬜ | `test_lifecycle.py` |
| Browsing | New, close, select, reorder (drag, keys), reopen closed tabs; links open next to their opener | ✅ | ⬜ | `test_browsing.py` (tab tests) |
| Browsing | Back, forward, reload, home; error pages keep the failed URL | ✅ | ⬜ | `test_back_forward_reload_home`, `test_error_page_is_shown_and_not_searched` |
| Browsing | Keyboard shortcuts (Windows bindings) | ✅ | 🟡 | `test_keyboard_shortcuts_*`; macOS bindings unit-tested only (`Shortcuts.MacConventions`) |
| Browsing | Find in page (count, next/previous, match case) | ✅ | ⬜ | `test_find_in_page` |
| Browsing | Zoom | ✅ | ⬜ | `test_zoom` |
| Browsing | Bookmarks: star, bar, manager page, persistence | ✅ | ⬜ | `test_places.py` (bookmark tests) |
| Browsing | History: recorded by TorFox only, search, remove, clear (overwritten in TorFox's files) | ✅ M | ⬜ | `test_places.py` (history tests) |
| Browsing | Clearing history also forgets per-site zoom levels | ❌ | ⬜ | `test_clearing_history_forgets_zoomed_sites` (expected failure; Stage 4) |
| Browsing | Native menus: app, star, bookmark folders, page context menu (no save/inspect items) | ✅ | 🟡 | `test_app_menu`, `test_web_context_menu_has_no_save_or_inspect`, via the menu test hook |
| Browsing | Address-bar suggestions | ⬜ | ⬜ | Stage 6 |
| Search | Typed queries go to DuckDuckGo; `.onion` input is never searched | ✅ | ⬜ | `test_search_goes_to_duckduckgo`, `test_onion_input_is_refused_and_never_searched`, `test_onion_input_is_never_searched_or_loaded` |
| Search | Selected text searched from the context menu; never if it mentions `.onion` | ✅ M | ⬜ | `test_selection_search_uses_duckduckgo`, `test_onion_selections_are_never_searched` |
| Downloads | Disable downloads blocks every vector (11 vectors; overrides Ask), in `CanDownload` and again in `OnBeforeDownload` | ✅ M | ⬜ | `test_disabled_blocks_every_vector`, `InnerLayerTest` (findings §3.10) |
| Downloads | Ask before download (default ON): native prompt with name, size, origin; cancel leaves no file | ✅ M | 🟡 | `test_ask_and_cancel_saves_nothing`, `test_ask_and_accept_saves_the_file`, `test_aa_a_new_profile_asks_first`; macOS sheet never compiled |
| Downloads | Unconfirmed bytes only in TorFox's staging folder, never in Downloads | ✅ M | ⬜ | `test_closing_the_tab_cancels_an_open_prompt` (findings §2) |
| Downloads | Turning downloads off cancels running downloads and open prompts | ✅ M | ⬜ | `test_disabling_cancels_running_downloads_and_prompts` |
| Downloads | Safe, unique file names; partial files never have the final name | ✅ M | ⬜ | `test_names_are_made_safe_and_unique`, `test_unfinished_files_never_have_their_final_name`, `DownloadFileName.*` |
| Downloads | Names fit the path limit; reserved while waiting; kept across Chromium's restarts | ✅ M | ⬜ | `DownloadPath.*`, `test_downloads_waiting_at_the_same_time_get_different_names`, `test_restarted_downloads_keep_their_name` (findings §3.5, §3.6) |
| Downloads | Failed downloads leave no partial file (TorFox's own clean-up as a backstop) | ✅ M | ⬜ | `test_failed_downloads_leave_nothing_behind`, `test_leftover_partial_files_are_removed` (findings §3.9) |
| Downloads | Chromium's own download records (normal profile `History`, `DownloadMetadata`) erased when Chromium is not running | ✅ M | ⬜ | `ChromiumProfileTest.*`, `test_download_records_are_erased`; present on disk while TorFox runs (findings §3.8) |
| Downloads | Mark of the Web kept on finished files | ✅ | ⬜ | `test_finished_files_are_marked_as_downloaded` |
| Downloads | Pages cannot write files another way (File System Access pickers off) | ✅ M | ⬜ | `test_file_system_access_pickers_are_absent` (findings §3.4) |
| Downloads | Downloads page (list, cancel, show in folder, remove, clear; no "open"), per-window for private windows | ✅ M | ⬜ | `test_downloads_page_*`, `test_private_downloads_stay_in_their_window` |
| Downloads | Drag-out of links/images; print to PDF | 🟡 | 🟡 | Not testable by automation (findings §6) |
| Privacy | Tracker blocking | ⬜ | ⬜ | Stage 4; WebSockets need another mechanism (findings §5) |
| Privacy | Third-party cookie blocking | ⬜ | ⬜ | Stage 4 |
| Privacy | HTTPS-first | ⬜ | ⬜ | Stage 4 |
| Privacy | Pop-ups without a user gesture blocked; click-opened pop-ups get no opener | ✅ | ⬜ | `test_popup_*` |
| Privacy | Pop-up per-site allow | ⬜ | ⬜ | Stage 4 |
| Privacy | Idle browser makes no network connections (20 s locally, 60 s on CI) | ✅ M | ⬜ | `test_idle_browser_makes_no_network_connections` |
| Privacy | Service contexts never auto-discover proxies | ✅ M | ⬜ | `test_service_contexts_never_autodiscover_proxies` |
| Privacy | No proxy auto-discovery (WPAD) at all | ❌ | ⬜ | Normal context auto-discovers once at start-up (findings §4) |
| Privacy | Chrome's `Default` profile holds no browsing data | ✅ | ⬜ | `test_no_history_in_default_profile` |
| Privacy | Private windows: own context each, isolated storage, nothing recorded or left on disk | ✅ M | ⬜ | `test_private.py` |
| Privacy | Clear data on exit | ⬜ | ⬜ | Stage 4 |
| Permissions | All permission requests denied (interim default) | ✅ | ⬜ | `test_permission_requests_are_denied`, `test_media_permission_refused_by_torfox` |
| Permissions | Per-site settings for camera, mic, location, notifications, clipboard, pop-ups | ⬜ | ⬜ | Stage 4 |
| Network | WebRTC leak protection | ⬜ | ⬜ | Spike D only |
| Tor | Regular tabs never resolve or connect to `.onion` (incl. WebSockets) | ✅ M | ⬜ | `OnionResolverTest`, `test_onion_subresources_and_navigation_are_blocked` |
| Tor | Bundled tor managed automatically | ⬜ | ⬜ | Stage 5 |
| Tor | Onion traffic only via Tor; no DNS leak; fail closed | ⬜ | ⬜ | Stage 5 (Spike C only) |
| Tor | Status indicator | ⬜ | ⬜ | Stage 5 |
| Settings | Store: atomic writes, validation, corrupt-file backup and reset | ✅ | ⬜ | `SettingsStore.*`, `SettingsSchema.*`, `test_settings.py` |
| Settings | Theme persists across restart | ✅ | ⬜ | `test_theme_persists_across_restart` |
| Settings | All 8 sections active and persistent | ⬜ | ⬜ | Sections exist and are labelled "planned"; Stage 6 |
| Updates | Signed GitHub Releases updates | ⬜ | ⬜ | Stage 7 |
| Packaging | Windows installer | ⬜ | — | Stage 8 |
| Packaging | macOS DMG | — | ⬜ | Stage 8 |
