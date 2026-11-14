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

Last updated: end of Stage 1 (2026-11-14). The evidence is in [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md).

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
| Browsing | Tab reorder and reopen, bookmarks, history, find, zoom | ⬜ | ⬜ | Stage 2 |
| Search | Typed queries go to DuckDuckGo; `.onion` input is never searched | ✅ | ⬜ | `test_search_goes_to_duckduckgo`, `test_onion_input_is_refused_and_never_searched` |
| Downloads | All downloads blocked (interim default until Stage 3) | ✅ M | ⬜ | `test_downloads_are_blocked` |
| Downloads | Ask before download (default ON) | ⬜ | ⬜ | Stage 3 |
| Downloads | Disable downloads blocks every vector | ⬜ | ⬜ | Stage 3 |
| Privacy | Tracker blocking | ⬜ | ⬜ | Stage 4; WebSockets need another mechanism (findings §5) |
| Privacy | Third-party cookie blocking | ⬜ | ⬜ | Stage 4 |
| Privacy | HTTPS-first | ⬜ | ⬜ | Stage 4 |
| Privacy | Pop-ups without a user gesture blocked; click-opened pop-ups get no opener | ✅ | ⬜ | `test_popup_*` |
| Privacy | Pop-up per-site allow | ⬜ | ⬜ | Stage 4 |
| Privacy | Idle browser makes no network connections (20 s locally, 60 s on CI) | ✅ M | ⬜ | `test_idle_browser_makes_no_network_connections` |
| Privacy | Service contexts never auto-discover proxies | ✅ M | ⬜ | `test_service_contexts_never_autodiscover_proxies` |
| Privacy | No proxy auto-discovery (WPAD) at all | ❌ | ⬜ | Normal context auto-discovers once at start-up (findings §4) |
| Privacy | Chrome's `Default` profile holds no browsing data | ✅ | ⬜ | `test_no_history_in_default_profile` |
| Privacy | Private browsing | ⬜ | ⬜ | Stage 2 |
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
