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

Last updated: Stage 11 (2026-11-15): the built-in ad blocker ([`STAGE11_FINDINGS.md`](STAGE11_FINDINGS.md)).
Stage 10 (automatic updates, What's new, Settings > Memory and security polish) is in
[`STAGE10_FINDINGS.md`](STAGE10_FINDINGS.md); all 23 of its mutations were run and caught (§5 there). The evidence is in [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md),
[`STAGE2_FINDINGS.md`](STAGE2_FINDINGS.md), [`STAGE3_FINDINGS.md`](STAGE3_FINDINGS.md),
[`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md), [`STAGE5_FINDINGS.md`](STAGE5_FINDINGS.md),
[`STAGE6_FINDINGS.md`](STAGE6_FINDINGS.md), [`STAGE7_FINDINGS.md`](STAGE7_FINDINGS.md),
[`STAGE8_FINDINGS.md`](STAGE8_FINDINGS.md) and [`STAGE9_FINDINGS.md`](STAGE9_FINDINGS.md). Unless a row
says otherwise, Tor rows are verified with a scripted tor (`tests/e2e/fake_tor.py`); *live* marks checks
made with the bundled tor on the real Tor network.

| Area | Feature | Windows | macOS | Evidence |
|---|---|---|---|---|
| Engine | Sandbox on for every process type, incl. network service (release build) | ✅ | 🟡 | `tests/platform/test_release_win.py`; macOS helpers initialise the sandbox, untested |
| Engine | Command-line switches can't weaken a release build | ✅ M | ⬜ | `test_command_line_cannot_weaken_torfox`, `CmdlinePolicy.*` |
| Engine | Release builds write no log files (Chromium, CEF or TorFox), even when switches ask for them; Chromium does not even create its (empty) `chrome_debug.log`. Exception: started with `--enable-logging`, CEF's launcher (`TorFox.exe`) writes its own two start-up lines, which name no page, to `debug.log` next to it (Stage 11 findings §9) | ✅ M | ⬜ | `test_release_builds_write_no_logs`, `CmdlinePolicy.ReleaseBuildsOpenNoLogFile` |
| Engine | Site isolation (`--site-per-process`): frames of other sites run in other renderer processes, in normal and private windows | ✅ M | 🟡 | `test_site_isolation.py` (with a same-site control); Tor windows not covered |
| UI isolation | `torfox://` unreachable from web content (all layers) | ✅ M | ⬜ | `test_isolation.py` (37 tests, several covering many URL variants; each guard layer on its own) |
| UI isolation | Bridge absent from web pages; every message validated | ✅ M | ⬜ | `test_isolation.py`, `test_bridge.py`, `BridgeValidator.*` |
| UI isolation | Internal pages: strict CSP, no eval or inline scripts, no frames | ✅ | ⬜ | `test_internal_pages.py` |
| UI isolation | Internal pages can't reach the web | ✅ M | ⬜ | `InternalPagesStayInternalTest` |
| UI isolation | The suggestion list can only name a row by position, and no tab can show its page | ✅ M | ⬜ | `test_the_list_cannot_do_anything_else`, `NavInput.InternalPages`, `BridgeValidator.*` |
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
| Browsing | Address-bar suggestions: matching bookmarks and history, never from the network; keyboard and mouse; hidden by Escape, blur and tab switches | ✅ | ⬜ | `test_suggestions.py`, `PlacesSuggest.*` |
| Browsing | Suggestions in Tor windows: bookmarks only | ✅ M | ⬜ | `test_tor.TorSuggestTest` (normal window as the control) |
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
| Privacy | Tracker and ad blocking for the requests a service worker makes itself, in normal, private and Tor windows | ✅ M | ⬜ | `WorkerTest`, `AdBlockTorWorkerTest` (Stage 11 findings §2.1) |
| Privacy | Ad blocking (EasyList, compiled in): ad requests cancelled before they are sent, in normal, private and Tor windows (setting) | ✅ M | ⬜ | `test_adblock.py`, `AdBlockTorTest`, `AdBlock.*` |
| Privacy | Ad blocking: `$third-party`, `domain=`, exceptions, `$important`, and page exceptions (`$document`, `$elemhide`, `$generichide`, `$genericblock`) | ✅ M | ⬜ | `AdBlock.*` (unit), `test_third_party_rules_leave_a_sites_own_requests_alone` |
| Privacy | Ad blocking never blocks a page the user opens (`$document` blocking rules are ignored) | ✅ M | ⬜ | `test_pages_are_never_blocked`, `AdBlock.RulesNotUnderstoodAreNeverApplied` |
| Privacy | Ad boxes hidden: every site's rules in the top-level document, a site's own rules in its frames; selectors that could escape their style rule are ignored | ✅ M | ⬜ | `AdBoxTest`, `AdBlockTorTest`, `AdBlock.ElementHiding`, `AdBlock.SelectorsThatCouldEscapeTheirRuleAreIgnored` |
| Privacy | Ad pop-ups (`$popup` rules) blocked, even after a click | ✅ M | ⬜ | `AdPopupTest` |
| Privacy | Shield in the toolbar: what was blocked on the page; allow ads on a site and block them again (saved for normal windows, kept in memory by each private or Tor window) | ✅ M | ⬜ | `ShieldMenuTest`, `PrivateWindowAdBlockTest`, `AdBlockTorTest` |
| Privacy | Ad blocking for WebSockets | ❌ | ⬜ | `test_ad_websockets_are_blocked` (expected failure): CEF does not show WebSockets to the request guard |
| Privacy | Hiding the boxes of blocked images and frames | ⬜ | ⬜ | Not done: a blocked image shows as a broken image unless a hiding rule covers its box (Stage 11 findings §3) |
| Privacy | uBlock Origin's own filter syntax: scriptlets (`##+js`), procedural filters, `$redirect`, `$csp`, `$removeparam`, regular expressions | ⛔ | ⛔ | Ignored rule by rule: TorFox applies only rules it fully understands (Stage 11 findings §1) |
| Privacy | Filter list updates between TorFox releases | ⛔ | ⛔ | The list is compiled in and changes only with a signed TorFox update; the ad blocker downloads nothing (Stage 11 findings §1) |
| Privacy | Third-party cookie blocking (setting applies at once) | ✅ M | ⬜ | `CookieTest` |
| Privacy | JavaScript optimizer (setting): off in Tor windows by default, or off everywhere; Chromium starts those renderers with `--js-flags=--disable-optimizing-compilers` | ✅ M | ⬜ | `test_js_optimizer.py` (each tab's renderer command line, with normal-window controls); what V8 does with the switch is Chromium's |
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
| Network | No DNS probe (google.com) after a page's name fails to resolve | ✅ M | ⬜ | `FailedLookupTest` (Stage 9 findings §1.7) |
| Tor | Regular tabs never resolve or connect to `.onion`, also written with a trailing dot (incl. WebSockets) | ✅ M | ⬜ | `OnionResolverTest`, `test_onion_subresources_and_navigation_are_blocked` |
| Tor | Bundled tor started, controlled (SAFECOOKIE, TAKEOWNERSHIP) and restarted after a crash | ✅ M | ⬜ | `test_torrc_and_control_session`, `TorFailsClosedTest`, `TorController.*`; live: `RealTorTest` (tor 0.4.9.16) |
| Tor | tor ends with TorFox, also when TorFox is killed (job object) | ✅ M | 🟡 | `test_tor_runs_in_a_job_and_exits_with_torfox`, `test_the_job_alone_ends_tor_if_torfox_is_killed`; macOS relies on `__OwningControllerProcess` and TAKEOWNERSHIP, untested there |
| Tor | Every Tor-window request goes to tor's SOCKS port with the host name; nothing direct; no lookup that could leave the computer | ✅ M | ⬜ | `test_tor_window_waits_for_tor_then_loads_through_tor_only`, `TorDnsTest` (with a control) |
| Tor | Fail closed while tor connects or after it stops: tab guard, context gate and proxy, each on its own; tor's old port is never used again | ✅ M | ⬜ | `TorWindowTest`, `TorFailsClosedTest`, `TorContextGateTest`, `ProxyLayerTest`, `ProxyLayerAfterDeathTest` |
| Tor | While tor has no port (and after New identity) the proxy points at TorFox's own loopback listener, which resets every connection; no other program can take it or be given its connections | ✅ M | 🟡 | `test_the_placeholder_port_is_torfoxs_own` (binds on 127.0.0.1 refused; a wildcard listener on the port receives nothing); macOS code never compiled |
| Tor | A Tor window's traffic leaves through Tor and reaches onion services | ✅ live | ⬜ | `LiveTorTest`: check.torproject.org says `IsTor`, DuckDuckGo's onion service loads |
| Tor | Onion addresses: v3 with checksum, typos and mixed input refused, opened in a Tor window from normal windows | ✅ M | ⬜ | `TorOnion.*`, `test_mistyped_onion_addresses_are_refused_and_never_searched`, `test_onion_addresses_from_a_normal_window_open_in_a_tor_window` |
| Tor | Tor windows refuse local addresses (incl. workers), other apps, camera/microphone/clipboard and private-window links | ✅ M | ⬜ | `test_tor_windows_refuse_this_computer_and_the_local_network`, `TorContextGateTest`, `TorSecureContextTest`, `test_link_menu_never_leaves_tor` |
| Tor | Tor windows: third-party cookies always blocked; system proxy setting ignored | ✅ M | ⬜ | `test_third_party_cookies_are_always_blocked` (with a control), `test_system_proxy_setting_never_changes_the_tor_context` |
| Tor | Tor windows have no WebRTC (no `RTCPeerConnection` in any frame), so WebRTC looks up no name outside Tor | ✅ M | ⬜ | `test_tor_windows_have_no_webrtc`, `test_webrtc_looks_up_no_name_for_tor_windows` (both with a normal-window control; Stage 9 findings §1.1) |
| Tor | Tor windows: WebRTC's UDP policy (second layer) | ✅ M (config) | ⬜ | `test_webrtc_in_tor_windows_sends_nothing_and_reveals_no_address`: the policy as read back from Chromium (Stage 6 findings §3.2) |
| Tor | Status indicator state (Connecting n%, Connected, Not connected with the reason); the *Try again* button is not tested | ✅ | ⬜ | `TorWindowTest`, `TorFailsClosedTest`, `test_custom_mode_without_bridges_never_starts_tor` |
| Tor | New identity: Tor windows closed, new context, `SIGNAL NEWNYM` | ✅ M | ⬜ | `NewIdentityTest` |
| Tor | New identity: the old context sends nothing more, not even from a service worker or a WebSocket | ✅ M | ⬜ | `test_after_new_identity_the_old_context_sends_nothing` (Stage 9 findings §1.2) |
| Tor | Bridges (none, built-in, custom): validated, written to `torrc`, tor restarted on change | ✅ M | ⬜ | `BridgeSettingsTest`, `TorBridges.*`, `Torrc.*` |
| Tor | Bridges over the real network | 🟡 | 🟡 | Not exercised: this machine reaches Tor without them (Stage 5 findings §6) |
| Tor | Profile folders whose names tor cannot read (8.3 path) | ✅ | — | `NonAsciiProfileTest`; live: `RealTorNonAsciiProfileTest` |
| Tor | Downloads in Tor windows | 🟡 | 🟡 | Same context as every request; no test downloads in a Tor window |
| Tor | Tor Browser's fingerprinting resistance; per-site circuit isolation | ⛔ | ⛔ | TorFox is not Tor Browser; circuits are per destination (`IsolateDestAddr`) because Chromium's SOCKS client has no authentication |
| Settings | Store: atomic writes, validation, corrupt-file backup and reset | ✅ | ⬜ | `SettingsStore.*`, `SettingsSchema.*`, `test_settings.py` |
| Settings | Theme (dark, light, system) applied live and persists across restart | ✅ | ⬜ | `test_theme_persists_across_restart`, `test_the_ui_control_changes_the_setting` |
| Settings | Every active setting persists across restart | ✅ | ⬜ | `test_every_active_setting_persists_across_restart` |
| Settings | 9 sections, every setting active: General, Search, Privacy & Security, Downloads, Tor, Appearance, Memory, Updates, About | ✅ | ⬜ | `test_only_implemented_settings_are_active`, `test_planned_settings_are_disabled_and_labelled` (none left) |
| Settings | Memory: TorFox's total, each tab (or tabs sharing a process), the browser, toolbars, helpers and tor; updated every 2 s while shown | ✅ | 🟡 | `test_memory.py` against Windows' process list, `MemoryReport.*`, `test_settings_memory_counts_tor_and_never_names_tor_tabs`; macOS (`libproc`) never compiled |
| Settings | Memory never names private or Tor tabs; a renderer can only mislabel TorFox's own helpers | ✅ M | ⬜ | `test_private_tabs_are_not_named`, `MemoryReport.PrivateAndTorTabsAreNotNamed`, `MemoryReport.WhatARendererSaysOnlyAttributesTorFoxsOwnHelpers` |
| Updates | Only releases signed with the compiled-in minisign key; other keys, legacy signatures, changed manifests and changed signed comments refused | ✅ M | 🟡 | `Minisign.*` (signatures made by the official minisign tool), `Release.Manifest*`, `test_a_release_signed_with_another_key_is_ignored`, `test_a_changed_manifest_or_signed_comment_is_ignored` |
| Updates | Only strictly newer releases; an older signed release cannot be replayed, and is reported | ✅ M | 🟡 | `Release.OnlyNewerReleasesAreOffered`, `test_older_and_same_versions_are_never_offered`, `test_an_older_signed_release_cannot_be_replayed` |
| Updates | No update is offered while the record of seen releases cannot be saved | ✅ M | 🟡 | `test_no_update_is_offered_while_the_record_cannot_be_saved` |
| Updates | Only GitHub's hosts, for every request and redirect; files only from the repository's release folder | ✅ M | 🟡 | `Release.OnlyGitHubsHostsAreAllowed`, `Release.LatestReleaseAnswer`, `test_downloads_follow_redirects_only_to_allowed_hosts`, `test_release_files_of_another_repository_are_not_used` |
| Updates | The file must match the signed size and SHA-256, after download and again before install; it is started by its real path while still held open | ✅ M | 🟡 | `test_a_file_that_does_not_match_the_signed_release_is_deleted`, `test_a_file_changed_after_its_download_is_not_installed` |
| Updates | Installed after TorFox exits (Windows: the installer is started after shutdown; macOS: the DMG is opened and the user copies the app) | ✅ | 🟡 | `test_a_signed_newer_release_is_checked_downloaded_and_installed_after_exit` (with a fake installer that records its arguments); the real setup with those arguments: `test_update_waits_for_torfox_then_installs_and_restarts_it` |
| Updates | Settings: off means no contact with GitHub; automatic checks at most once a day; notices can be turned off | ✅ M | 🟡 | `test_with_updates_off_torfox_never_contacts_github`, `test_automatic_checks_*`, `test_notices_can_be_turned_off` |
| Updates | Automatic updates (default): a verified update downloads by itself; *Relaunch to update* in the toolbar installs it and starts TorFox again; with the setting off nothing is downloaded or installed unasked | ✅ M | 🟡 | `AutoUpdateTest.test_an_update_downloads_by_itself_and_the_toolbar_offers_a_relaunch` (button clicked), `test_with_automatic_updates_off_closing_torfox_installs_nothing`, `test_turning_automatic_updates_on_downloads_the_update_found`; macOS opens the DMG instead |
| Updates | A relaunch keeps the normal windows' tabs once; asks first if private or Tor windows would close; keeps nothing while history is cleared on exit | ✅ M | ⬜ | `AutoUpdateTest` (`test_private_windows_are_asked_about_before_a_relaunch`, `test_nothing_is_kept_for_a_relaunch_while_history_is_cleared_on_exit`, `test_a_relaunch_marker_without_an_update_restores_only_once`) |
| Updates | Closing TorFox with an update waiting installs it silently without starting TorFox; not while Windows ends the session | ✅ M | — | `test_an_update_waiting_when_torfox_closes_installs_without_a_relaunch`, `test_an_update_is_not_installed_while_windows_shuts_down` (through a test switch: a real shutdown is not tested) |
| Updates | A TorFox started while an update installs exits before CEF starts and is started by the setup afterwards | ✅ M | — | `UpdateBeingInstalledTest`, installer: `test_torfox_opened_during_an_update_starts_once_it_has_finished` |
| Updates | No gap between TorFox and the setup: TorFox holds the setup's mutex from before Chromium shuts down until the setup says it holds it too | ✅ M | — | the fake installer's record in `test_an_update_downloads_by_itself_…` and `test_an_update_waiting_when_torfox_closes_…` |
| Updates | After *Relaunch to update* the setup's own start entry starts TorFox again; a setup that gave up because TorFox never closed does not start another one | ✅ M | — | installer: `test_update_waits_for_torfox_then_installs_and_restarts_it` (which of the setup's two ways started it, from its log), `test_update_gives_up_when_torfox_stays_open` |
| Updates | A start that races the setup's finish starts after all; a *Relaunch to update* whose setup failed still starts TorFox | 🟡 | — | not tested: the first race lasts microseconds, and failing the setup takes a minute |
| Updates | What's new opens once after an update, with notes built into TorFox, shown as text; never for a new profile, a downgrade or a damaged record | ✅ M | 🟡 | `test_whats_new.py`, `WhatsNew.*` |
| Updates | What's new says the update was checked against the signing key only if TorFox installed that version itself; an update that did not install is reported at the next start | ✅ M | — | `test_an_update_torfox_installed_itself_is_said_to_be_checked`, `test_an_update_that_did_not_install_is_reported_once`, `WhatsNew.HandoffRecordTellsWhetherTheUpdateInstalled`; macOS shows neither (the user installs the app) |
| Updates | Only the settings page can use the updater; the toolbar can only ask for the relaunch of a ready update | ✅ | ⬜ | `test_only_the_settings_page_can_use_the_updater` |
| Updates | Builds without a release key never look for updates (every build until the maintainer adds the key) | ✅ | 🟡 | `test_a_build_without_a_key_never_looks_for_updates` |
| Updates | Release builds ignore the test update source | ✅ | — | `test_release_builds_ignore_the_test_update_source` |
| Updates | Updating from the real GitHub | 🟡 | 🟡 | No release has been signed with the official key yet; tested against a fake GitHub |
| Updates | Operating-system code-signature check of updates | ⛔ | ⛔ | No code-signing certificates (TorFox is not distributed through app stores); updates rest on the minisign signature |
| Packaging | Windows setup: installs for the current user without administrator rights; contents limited to an allowlist, Start-menu shortcut, uninstall entry, uninstall removes everything but the profile | ✅ M | — | `test_install_contents_shortcut_and_uninstall` for the real setup in its default folder and for the test build (Setup's log: non-administrative mode, `HKEY_CURRENT_USER`) |
| Packaging | The installed TorFox runs sandboxed | ✅ | — | `test_installed_torfox_runs_sandboxed` |
| Packaging | Update setup waits for TorFox to close, installs and restarts it with no prompt, and removes stale optional files; gives up without changes if TorFox stays open | ✅ M | — | `test_update_waits_for_torfox_then_installs_and_restarts_it`, `test_update_gives_up_when_torfox_stays_open` (the setup is started with the arguments TorFox's updater uses) |
| Packaging | Setup and uninstaller refuse while TorFox is open | ✅ M | — | `test_setup_and_uninstall_refuse_while_torfox_is_open` |
| Packaging | Update after TorFox closed (`/TORFOXNORELAUNCH`) installs without starting TorFox | ✅ | — | `test_an_update_after_torfox_closed_installs_without_starting_it` |
| Packaging | macOS DMG: ad-hoc signed app, mounts, copies out and starts | — | 🟡 | `packaging/macos/make_dmg.sh` and `check_dmg.sh` run on CI; never run yet (no Mac here, CI has not run) |
| Packaging | Microsoft (Authenticode) and Apple (Developer ID, notarization) signing | ⛔ | ⛔ | Not distributed through stores; SmartScreen and Gatekeeper warn on first install ([`INSTALL.md`](INSTALL.md)) |
| Release | Release workflow: fails closed without the tag, key, notes or secret; only tags on `main`; the secret is readable only by a sign job in the `release` environment that gets nothing but the manifest; actions pinned by commit; checks the signed release as TorFox will before publishing | 🟡 | 🟡 | `tests/tools/test_release.py` (manifest, check, preflight with the official minisign; the workflow's jobs, secret use and pins); the workflow itself has never run, and the environment's reviewers and tag rule are GitHub settings no test can see |
