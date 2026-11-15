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

Last updated: end of Stage 8 (2026-11-15). The evidence is in [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md),
[`STAGE2_FINDINGS.md`](STAGE2_FINDINGS.md), [`STAGE3_FINDINGS.md`](STAGE3_FINDINGS.md),
[`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md), [`STAGE5_FINDINGS.md`](STAGE5_FINDINGS.md),
[`STAGE6_FINDINGS.md`](STAGE6_FINDINGS.md), [`STAGE7_FINDINGS.md`](STAGE7_FINDINGS.md) and
[`STAGE8_FINDINGS.md`](STAGE8_FINDINGS.md). Unless a row
says otherwise, Tor rows are verified with a scripted tor (`tests/e2e/fake_tor.py`); *live* marks checks
made with the bundled tor on the real Tor network.

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
| Browsing | Clearing history also forgets per-site zoom levels | ✅ M | ⬜ | `test_clearing_history_forgets_zoomed_sites` (Stage 4 findings §3.5) |
| Browsing | Native menus: app, star, bookmark folders, page context menu (no save/inspect items) | ✅ | 🟡 | `test_app_menu`, `test_web_context_menu_has_no_save_or_inspect`, via the menu test hook |
| Browsing | Address-bar suggestions | ⬜ | ⬜ | Not built: needs a pop-up over the page (Stage 6 findings §2) |
| Browsing | On startup: restore previous tabs (normal windows; after a normal exit and a crash); never private or Tor windows, never while history is cleared on exit | ✅ M | ⬜ | `test_startup.py`, `Session.*` |
| Search | Search engine setting (DuckDuckGo, HTML, Lite), on the onion service in Tor windows | ✅ M | ⬜ | `SearchEngineTest`, `test_searches_in_tor_windows_use_duckduckgos_onion_service`, `Search.*` |
| Search | Typed queries go to DuckDuckGo (its onion service in Tor windows); `.onion` input is never searched | ✅ M | ⬜ | `test_search_goes_to_duckduckgo`, `test_searches_in_tor_windows_use_duckduckgos_onion_service`, `test_mistyped_onion_addresses_are_refused_and_never_searched` |
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
| Privacy | Tracker blocking (EasyPrivacy domains, third-party requests only; setting) | ✅ M | ⬜ | `TrackerTest`, `Trackers.*` |
| Privacy | Tracker blocking for WebSockets | ❌ | ⬜ | CEF does not show WebSockets to the request guard (Stage 4 findings §6) |
| Privacy | Third-party cookie blocking (setting applies at once) | ✅ M | ⬜ | `CookieTest` |
| Privacy | HTTPS-first: upgrade, question before http, per-session exceptions, no redirect loop (setting) | ✅ M | ⬜ | `HttpsFirstTest`, `HttpsFirst.*` |
| Privacy | HTTPS-first for form posts | ⛔ | ⛔ | Not upgraded: repeating a POST would resend the form (Stage 4 findings §1) |
| Privacy | Pop-ups without a click blocked with a notice; click-opened pop-ups open as tabs, no opener (setting) | ✅ M | ⬜ | `PopupTest`, `test_popup_*` |
| Privacy | Pop-up per-site allow | ⛔ | ⛔ | Not offered: a list of sites is a browsing record (Stage 4 findings §2) |
| Privacy | Idle browser makes no network connections (20 s locally, 60 s on CI) | ✅ M | ⬜ | `test_idle_browser_makes_no_network_connections` |
| Privacy | Service contexts never auto-discover proxies | ✅ M | ⬜ | `test_service_contexts_never_autodiscover_proxies` |
| Privacy | No proxy auto-discovery (WPAD) unless *Use the system's proxy settings* is on | ✅ M | ⬜ | `test_idle_browser_does_not_autodiscover_proxies`, `SystemProxyTest` (Stage 4 findings §3.6) |
| Privacy | Chrome's `Default` profile holds no browsing data | ✅ | ⬜ | `test_no_history_in_default_profile` |
| Privacy | Chromium's per-site scores never kept in the profile | ✅ M | ⬜ | `test_control_without_clearing_everything_is_kept`, `ChromiumProfileTest.*` (Stage 4 findings §3.5) |
| Privacy | Private windows: own context each, isolated storage, nothing recorded or left on disk | ✅ M | ⬜ | `test_private.py` |
| Privacy | Clear on exit (history, cookies and site data, cache), after a normal exit and after a crash; links never followed | ✅ M | ⬜ | `ClearOnExitTest`, `ClearData.*` |
| Privacy | Email and phone links ask first, after a click only; other schemes never reach the OS | ✅ M | ⬜ | `ExternalProtocolTest`, `ExternalProtocols.*` |
| Privacy | Handing the link to the mail or phone app | 🟡 | 🟡 | Test builds log instead of opening an app (Stage 4 findings §6) |
| Permissions | Camera, microphone, clipboard: Block (default) or Ask every time; no answer remembered | ✅ M | ⬜ | `PermissionTest` |
| Permissions | Location, notifications, screen capture and all others always denied (shown as *Always blocked* in Settings) | ✅ | ⬜ | `test_location_and_notifications_are_always_denied`, `test_screen_capture_is_always_denied`, `test_permission_requests_are_denied` |
| Network | WebRTC exposes no local addresses | ✅ | ⬜ | `WebRtcTest` (two layers; the test shows the outcome, Stage 4 findings §6) |
| Tor | Regular tabs never resolve or connect to `.onion` (incl. WebSockets) | ✅ M | ⬜ | `OnionResolverTest`, `test_onion_subresources_and_navigation_are_blocked` |
| Tor | Bundled tor started, controlled (SAFECOOKIE, TAKEOWNERSHIP) and restarted after a crash | ✅ M | ⬜ | `test_torrc_and_control_session`, `TorFailsClosedTest`, `TorController.*`; live: `RealTorTest` (tor 0.4.9.16) |
| Tor | tor ends with TorFox, also when TorFox is killed (job object) | ✅ M | 🟡 | `test_tor_runs_in_a_job_and_exits_with_torfox`, `test_the_job_alone_ends_tor_if_torfox_is_killed`; macOS relies on `__OwningControllerProcess` and TAKEOWNERSHIP, untested there |
| Tor | Every Tor-window request goes to tor's SOCKS port with the host name; nothing direct; no lookup that could leave the computer | ✅ M | ⬜ | `test_tor_window_waits_for_tor_then_loads_through_tor_only`, `TorDnsTest` (with a control) |
| Tor | Fail closed while tor connects or after it stops: tab guard, context gate and proxy, each on its own | ✅ M | ⬜ | `TorWindowTest`, `TorFailsClosedTest`, `TorContextGateTest`, `ProxyLayerTest`, `ProxyLayerAfterDeathTest` |
| Tor | A Tor window's traffic leaves through Tor and reaches onion services | ✅ live | ⬜ | `LiveTorTest`: check.torproject.org says `IsTor`, DuckDuckGo's onion service loads |
| Tor | Onion addresses: v3 with checksum, typos and mixed input refused, opened in a Tor window from normal windows | ✅ M | ⬜ | `TorOnion.*`, `test_mistyped_onion_addresses_are_refused_and_never_searched`, `test_onion_addresses_from_a_normal_window_open_in_a_tor_window` |
| Tor | Tor windows refuse local addresses (incl. workers), other apps, camera/microphone/clipboard and private-window links | ✅ M | ⬜ | `test_tor_windows_refuse_this_computer_and_the_local_network`, `TorContextGateTest`, `TorSecureContextTest`, `test_link_menu_never_leaves_tor` |
| Tor | Tor windows: third-party cookies always blocked; system proxy setting ignored | ✅ M | ⬜ | `test_third_party_cookies_are_always_blocked` (with a control), `test_system_proxy_setting_never_changes_the_tor_context` |
| Tor | Tor windows: WebRTC sends no UDP and gathers no candidate | ✅ M (config) | ⬜ | `test_webrtc_in_tor_windows_sends_nothing_and_reveals_no_address`; on this machine no window sends WebRTC traffic, so the Tor policy is checked as read back from Chromium (Stage 6 findings §3.2) |
| Tor | Status indicator state (Connecting n%, Connected, Not connected with the reason); the *Try again* button is not tested | ✅ | ⬜ | `TorWindowTest`, `TorFailsClosedTest`, `test_custom_mode_without_bridges_never_starts_tor` |
| Tor | New identity: Tor windows closed, new context, `SIGNAL NEWNYM` | ✅ M | ⬜ | `NewIdentityTest` |
| Tor | Bridges (none, built-in, custom): validated, written to `torrc`, tor restarted on change | ✅ M | ⬜ | `BridgeSettingsTest`, `TorBridges.*`, `Torrc.*` |
| Tor | Bridges over the real network | 🟡 | 🟡 | Not exercised: this machine reaches Tor without them (Stage 5 findings §6) |
| Tor | Profile folders whose names tor cannot read (8.3 path) | ✅ | — | `NonAsciiProfileTest`; live: `RealTorNonAsciiProfileTest` |
| Tor | Downloads in Tor windows | 🟡 | 🟡 | Same context as every request; no test downloads in a Tor window |
| Tor | Tor Browser's fingerprinting resistance; per-site circuit isolation | ⛔ | ⛔ | TorFox is not Tor Browser; circuits are per destination (`IsolateDestAddr`) because Chromium's SOCKS client has no authentication |
| Settings | Store: atomic writes, validation, corrupt-file backup and reset | ✅ | ⬜ | `SettingsStore.*`, `SettingsSchema.*`, `test_settings.py` |
| Settings | Theme (dark, light, system) applied live and persists across restart | ✅ | ⬜ | `test_theme_persists_across_restart`, `test_the_ui_control_changes_the_setting` |
| Settings | Every active setting persists across restart | ✅ | ⬜ | `test_every_active_setting_persists_across_restart` |
| Settings | 8 sections, every setting active: General, Search, Privacy & Security, Downloads, Tor, Appearance, Updates, About | ✅ | ⬜ | `test_only_implemented_settings_are_active`, `test_planned_settings_are_disabled_and_labelled` (none left) |
| Updates | Only releases signed with the compiled-in minisign key; other keys, legacy signatures, changed manifests and changed signed comments refused | ✅ | 🟡 | `Minisign.*` (signatures made by the official minisign tool), `Release.Manifest*`, `test_a_release_signed_with_another_key_is_ignored`, `test_a_changed_manifest_or_signed_comment_is_ignored` |
| Updates | Only strictly newer releases; an older signed release cannot be replayed | ✅ | 🟡 | `Release.OnlyNewerReleasesAreOffered`, `test_older_and_same_versions_are_never_offered`, `test_an_older_signed_release_cannot_be_replayed` |
| Updates | Only GitHub's hosts, for every request and redirect; files only from the repository's release folder | ✅ | 🟡 | `Release.OnlyGitHubsHostsAreAllowed`, `Release.LatestReleaseAnswer`, `test_downloads_follow_redirects_only_to_allowed_hosts`, `test_release_files_of_another_repository_are_not_used` |
| Updates | The file must match the signed size and SHA-256, after download and again before install | ✅ | 🟡 | `test_a_file_that_does_not_match_the_signed_release_is_deleted`, `test_a_file_changed_after_its_download_is_not_installed` |
| Updates | Installed after TorFox exits (Windows: the installer is started after shutdown; macOS: the DMG is opened and the user copies the app) | ✅ | 🟡 | `test_a_signed_newer_release_is_checked_downloaded_and_installed_after_exit` (with a fake installer that records its arguments); the real setup with those arguments: `test_update_waits_for_torfox_then_installs_and_restarts_it` |
| Updates | Settings: off means no contact with GitHub; automatic checks at most once a day; notices can be turned off | ✅ | 🟡 | `test_with_updates_off_torfox_never_contacts_github`, `test_automatic_checks_*`, `test_notices_can_be_turned_off` |
| Updates | Only the settings page can use the updater | ✅ | ⬜ | `test_only_the_settings_page_can_use_the_updater` |
| Updates | Builds without a release key never look for updates (every build until the maintainer adds the key) | ✅ | 🟡 | `test_a_build_without_a_key_never_looks_for_updates` |
| Updates | Release builds ignore the test update source | ✅ | — | `test_release_builds_ignore_the_test_update_source` |
| Updates | Updating from the real GitHub | 🟡 | 🟡 | No release has been signed with the official key yet; tested against a fake GitHub |
| Updates | Operating-system code-signature check of updates | ⛔ | ⛔ | No code-signing certificates (TorFox is not distributed through app stores); updates rest on the minisign signature |
| Packaging | Windows setup: contents limited to an allowlist, Start-menu shortcut, uninstall entry, uninstall removes everything but the profile | ✅ per user | — | `test_install_contents_shortcut_and_uninstall` (per-user test build, locally). The per-machine setup runs the same tests on CI with administrator rights; CI has not run yet |
| Packaging | The installed TorFox runs sandboxed | ✅ per user | — | `test_installed_torfox_runs_sandboxed` |
| Packaging | Update setup waits for TorFox to close, installs and restarts it; gives up without changes if TorFox stays open | ✅ per user | — | `test_update_waits_for_torfox_then_installs_and_restarts_it`, `test_update_gives_up_when_torfox_stays_open` (the setup is started with the arguments TorFox's updater uses) |
| Packaging | Setup and uninstaller refuse while TorFox is open | ✅ per user | — | `test_setup_and_uninstall_refuse_while_torfox_is_open` |
| Packaging | macOS DMG: ad-hoc signed app, mounts, copies out and starts | — | 🟡 | `packaging/macos/make_dmg.sh` and `check_dmg.sh` run on CI; never run yet (no Mac here, CI has not run) |
| Packaging | Microsoft (Authenticode) and Apple (Developer ID, notarization) signing | ⛔ | ⛔ | Not distributed through stores; SmartScreen and Gatekeeper warn on first install ([`INSTALL.md`](INSTALL.md)) |
| Release | Release workflow: fails closed without the tag, key, notes or secret; signs with the official minisign; checks the signed release as TorFox will before publishing | 🟡 | 🟡 | `tests/tools/test_release.py` (manifest, check and preflight, with the official minisign); the workflow itself has never run |
