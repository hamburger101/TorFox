# Stage 6 findings: settings and appearance

Stage 6 makes every section of Settings do what it says, or say plainly what it does not do yet. Results:

- **Unit tests:** 159 tests: 158 pass, 1 skipped (`ClearData.NeverFollowsLinks`, which needs permission to create symbolic links; unchanged since Stage 4).
- **End-to-end tests:** 213 tests (test build, no network): all pass, 3 skipped (the tests that use the real Tor network, run separately). The first full run had one error, a bug in the new WebRTC test itself (a keyword clash in `events()`); after the fix that test passes.
- **Mutation check:** all 90 mutations (Stages 1–6) detected, including the 6 new ones and the Stage 5 survivor.

## 1. What was built
- **General > On startup.** *Open a new tab* (the default) or *Restore previous tabs*. Restoring reopens the
  tabs of every normal window, the selected tab of each, after a normal exit and after a crash. An address
  TorFox is started with opens after the first window's tabs. Private and Tor windows are never kept. See
  [`ARCHITECTURE.md`](ARCHITECTURE.md#browsing-data) for the file and its rules.
- **Search Engine.** DuckDuckGo, DuckDuckGo HTML or DuckDuckGo Lite, used at once by the address bar, the
  new tab page and selection search; in Tor windows the same page on DuckDuckGo's onion service (its `/html/`
  and `/lite/` pages were checked live over Tor during Stage 5).
- **Privacy & Security > Site permissions.** Location and notifications were settings marked "not active
  yet". TorFox always refuses both (Stage 4), so they are no longer settings: the page shows them as
  *Always blocked*. Old settings files that still name them are repaired on load (unknown keys are dropped).
- **About.** The bundled Tor Expert Bundle version, and tor's own version once Tor has started.
- **New tab page.** Its three notes still described the Stage 1 build ("Tor is not available yet",
  "Downloads are blocked"). They now describe what TorFox does.
- **Updates.** Still marked *not active yet*: the updater is Stage 7.

## 2. Changes from the plan
- **Address-bar suggestions are not built.** The suggestion list needs a pop-up that covers the page (an
  overlay view with its own internal page). That was set aside to give the updater and the installers
  (Stages 7 and 8) the time they need. Suggestions are marked *Not started* in
  [`FEATURE_STATUS.md`](FEATURE_STATUS.md). `Places::Suggest` (history and bookmarks only, never the
  network) exists and is unit-tested from Stage 2.
- **Location and notifications are not settings** (§1).

## 3. Findings
### 3.1 Tabs created while a window is being shown stay hidden
A window's first tab already loaded its start address only after the window was shown (Stage 5 found that
a browser created before then stays "hidden" to Chromium). Restoring a session showed the same problem for
the other tabs created during window creation: the selected restored tab reported
`document.visibilityState` "hidden" after it was selected. Chromium throttles hidden pages and permission
prompts wait for them. Every first tab and every restored tab is now created by a task that runs after
the window is shown. `test_restore_reopens_the_tabs_after_a_normal_exit` checks the restored page is
visible, and a mutation that creates them during window creation is detected.

### 3.2 Tor windows' WebRTC policy: configuration, not behaviour, on this machine
The Stage 5 mutation check showed the Tor-window WebRTC test passed with Tor windows' WebRTC policy
(`disable_non_proxied_udp`) replaced by the normal windows' one. The test's STUN server could not be
reached either way, so it could not tell them apart. The test now gives the page a STUN server it can reach
(on this computer's default-route address) and counts every UDP packet that arrives. As a control, a normal
window's page gathers too. **On this machine even the control sends nothing and gathers no candidate at all**
(also without a STUN server), so behaviour cannot show the difference here. The test therefore also checks
the policy Chromium read back for the Tor context, which is a configuration check: TorFox already refuses to
open a Tor window if that read-back fails. On a machine where WebRTC does gather, the packet count is the
behavioural check. The test prints how many packets the control sent.

### 3.3 Stale text on the new tab page
The new tab page's notes were written in Stage 1 and never updated: they said Tor and downloads did not
work. Nothing tested them. They are now accurate, and Stage 9's review covers every user-facing text.

## 4. How it is tested
| Test | Shows |
|---|---|
| `Search.*` (unit) | Each engine's page, on the clearnet and the onion service; other URLs untouched; unknown engines get the default |
| `Session.*` (unit) | Which URLs are kept (no onion hosts, no `javascript:`/`file:`, no shell page); round trip; damaged, foreign, over-limit files |
| `SearchEngineTest` | Each choice gets the next search from the address bar, and from the new tab page's search box |
| `test_searches_in_tor_windows_use_duckduckgos_onion_service` | Each choice, in a Tor window, on the onion service; never `duckduckgo.com` |
| `test_startup.py` | §1 On startup, across real restarts (normal exit and crash) |
| `test_every_active_setting_persists_across_restart` | Every active setting, set through the settings page to a value other than its default, reads back after a restart; tor then starts at launch |
| `test_only_implemented_settings_are_active`, `test_settings_service_refusals` | Which settings are active; refused values, removed keys and planned settings |

## 5. Mutation check
Stage 6 adds 6 mutations: the search engine choice ignored; restored sessions keeping private windows,
being kept while history is cleared on exit, or keeping onion addresses; the session file trusted without
checks; and first tabs created while the window is being shown (§3.1).

The full run on the Stage 6 commit applied all 90 mutations of Stages 1–6 one at a time. **All 90 were
detected.** That includes `tor-webrtc-not-restricted`, which survived in Stage 5 and is now detected by the
policy check (§3.2).

## 6. Gaps
- Address-bar suggestions (§2).
- Restored tabs all load at once; there is no lazy loading of background tabs.
- Window size and position are not restored.
- macOS: nothing in this stage has been compiled or run on macOS.
