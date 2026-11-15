# Testing TorFox

TorFox treats a protection as working only when an automated test shows its behaviour. A test that only
checks the interface doesn't count. [`FEATURE_STATUS.md`](FEATURE_STATUS.md) is updated only from tests
described here.

## Builds
There are two build trees, and both are Release. CEF's Debug build trips an assertion in off-the-record
profiles, so it isn't used.

| Tree | Configure | Used by |
|---|---|---|
| `out/dev` | `cmake -S . -B out/dev -G "Visual Studio 18 2026" -A x64` | Release-build platform tests, manual use |
| `out/test` | the same, plus `-DTORFOX_TEST_HOOKS=ON` | Unit, end-to-end and mutation tests |

```
cmake --build out/dev --config Release --parallel
cmake --build out/test --config Release --parallel
```
Use the CMake bundled with Visual Studio. CI adds `-DTORFOX_CEF_FLAVOR=minimal` to download the smaller CEF
distribution. The result is the same.

### What test hooks change
`TORFOX_TEST_HOOKS=ON` keeps these switches, which release builds drop:

| Switch | Purpose |
|---|---|
| `--remote-debugging-port` (loopback only) | DevTools access for the E2E tests |
| `--log-net-log`, `--net-log-capture-mode` | NetLog for the network tests |
| logging switches | Debug logging |
| `--torfox-test-log` | A JSON-lines log of security decisions (`nav_blocked`, `resource_blocked`, `scheme_request`, `bridge_reject`, `permission_denied`, …) |
| `--torfox-test-user-data` | Throw-away profile |
| `--torfox-test-host-rules` | Maps fixture hostnames to local servers |
| `--torfox-test-start-url` | Start URL |
| `--torfox-test-skip-guards=nav[,resource][,download][,download-update]` | Fault injection: turns off TorFox's navigation guard, resource guard, `CanDownload` refusal or the per-update download cancel, so the layers they hide can be tested on their own |
| `--torfox-test-menu-choice=<file>` | Native menus: instead of showing a menu, log its items (`menu_shown`) and pick the item whose label path (`Folder/Item`) is in the file. The file is deleted each time; no file means the menu is dismissed |
| `--torfox-test-dialog-choice=<file>` | Native prompts (the download prompt) and file dialogs: log what would be shown (`dialog_shown`) and answer from the file: `accept`, `cancel`, `hold` (leave it open), or `file:<path>` for a file dialog. Consumed each time; no file means the real dialog |
| `--torfox-test-download-dir=<dir>` | Download folder used instead of the user's Downloads folder |
| `--torfox-test-https-port=<http>:<https>` | HTTPS-first: fixture servers listen on random ports, so an `http://` URL on port `<http>` is upgraded to port `<https>` (other explicit ports are never upgraded) |
| `--ignore-certificate-errors-spki-list` | Trusts the fixture servers' self-signed certificate (by key hash) for the https tests |
| `--use-fake-device-for-media-stream` | A fake camera and microphone for the permission tests |
| `--disable-backgrounding-occluded-windows` | Passed to every test: a test window covered by other windows would otherwise count as hidden, and input that waits for a frame stalls ([`STAGE7_FINDINGS.md`](STAGE7_FINDINGS.md) §3.5) |
| `--torfox-test-update-config=<file>` | Points the updater at a fake GitHub with its own key ([`tests/e2e/test_updates.py`](../tests/e2e/test_updates.py)) |
| `--torfox-test-session-ending` | Makes TorFox behave as if Windows were ending the session when it closes, so a waiting update is not installed |

None of this exists in a release build. The release platform test passes these switches and checks that they
have no effect.

## Unit tests
```
ctest --test-dir out/test -C Release --output-on-failure
```
GoogleTest, in `tests/unit/`, covering:
- the settings store;
- bridge message validation;
- the command-line policy;
- the `torfox://` URL rules and guards;
- JSON parsing;
- navigation input and the Public Suffix List;
- history and bookmarks storage (`places.db`), closed tabs and zoom steps;
- keyboard shortcuts (Windows and macOS tables) and context-menu search rules.
- the updater: minisign signatures checked against signatures made by the official minisign tool
  (`tests/unit/data/update`), the release manifest rules, version order, replay, the GitHub host
  allowlist, the saved update state and SHA-256.

## End-to-end tests (Windows)
```
cd tests/e2e
python -m unittest -v test_isolation test_bridge test_internal_pages test_settings test_lifecycle test_stage1_defaults test_quiet_startup test_browsing test_places test_private test_downloads test_privacy test_tor test_startup test_updates test_site_isolation test_suggestions test_whats_new test_memory test_js_optimizer
```
Each test starts the test build (`out/test/...`, or set `TORFOX_TEST_EXE`) with a fresh temporary profile.
It drives TorFox over DevTools on loopback and serves fixtures from local HTTP servers. No internet access
is needed, and none is used: the Tor tests use a scripted tor (`fake_tor.py`).

The tests that run the bundled tor use the Tor network, so they run only when asked:
```
TORFOX_LIVE_TESTS=1 python -m unittest -v test_tor.RealTorTest test_tor.RealTorNonAsciiProfileTest test_tor.LiveTorTest
```
`LiveTorTest` is skipped, not failed, if tor cannot finish bootstrapping from where it runs.

| File | Covers |
|---|---|
| `test_isolation.py` | Every web→`torfox://` vector: subresources, workers, frames, navigations, pop-ups, forms, redirects. Also browser-initiated navigations, each guard layer on its own, the bridge being absent from web pages, and internal pages staying internal |
| `test_bridge.py` | Malformed, oversized, duplicate-key and wrong-page bridge messages. A rejected message changes nothing |
| `test_internal_pages.py` | Security headers, strict CSP, eval and inline scripts blocked, Trusted Types, no subframes |
| `test_settings.py` | Persistence across restart (every active setting, through the settings page), corrupt-file recovery, invalid values falling back to safe defaults |
| `test_startup.py` | Stage 6: *On startup*: a new tab by default with no session file; *Restore previous tabs* after a normal exit and after a crash (several windows, the selected tab, a start address added after them, restored pages visible); private windows never kept; nothing kept while history is cleared on exit; switching back removes the file; damaged files open a new tab |
| `test_lifecycle.py` | Opening and closing tabs and windows, and replacing a browser when its role changes |
| `test_stage1_defaults.py` | Permissions denied, onion refused and never searched, pop-ups only on a click, DuckDuckGo search, no data in Chrome's `Default` profile |
| `test_quiet_startup.py` | NetLog of an idle session: no connections and no proxy auto-discovery from any context (with a control that turns the system-proxy setting on). Onion names, also with a trailing dot, never reach the resolver. A page whose name does not resolve makes no other lookup (Chromium's DNS probe is off) |
| `test_browsing.py` | Address bar and DuckDuckGo URLs (and each *Search engine* choice, `SearchEngineTest`), onion input, navigation controls, error pages, tabs (new, close, reopen, reorder, drag), keyboard shortcuts, find, zoom, context and app menus |
| `test_places.py` | History recorded only by TorFox (Chromium's `History` stays empty), History page search/remove/clear leaving no bytes behind, bookmarks star/bar/manager and persistence |
| `test_private.py` | Private windows: marking, no history, storage isolated from normal and other private windows, links stay private, nothing left on disk (with a control) |
| `test_privacy.py` | Stage 4: trackers, HTTPS-first, third-party cookies, pop-ups, site permissions, email and phone links, WebRTC addresses, and clear-on-exit across real restarts (normal exit, crash, and a control). Fixture sites get real hostnames (`sites.py`) so that site and public-suffix rules apply |
| `test_tor.py` | Stage 5, with a scripted tor (`fake_tor.py`) that implements tor's control port and records every SOCKS request: Tor windows wait for tor, then load only through it (host names, no local lookups); the `torrc` and control session; onion addresses and searches; local addresses, other apps, permissions, link menus, cookies and WebRTC in Tor windows (none in any frame, and no name WebRTC would look up reaches the resolver); tor dying and restarting, and its old port never used again; each fail-closed layer on its own; service workers; new identity, after which the old context sends nothing; the job object; bridges; non-ASCII profile folders |
| `test_updates.py` | Stage 7, with a fake GitHub (a local server; real builds accept only GitHub's https hosts) and throw-away minisign keys made per run by `scripts/minisign.py`: a signed newer release is found, downloaded, checked and handed to the installer after TorFox exits (a fake installer records how it was started); releases signed with another key, changed manifests or signed comments, files that do not match the signed size and SHA-256 (at download and again just before install), older and same versions, replayed older releases (reported as such), redirects to other hosts and files of other repositories are all refused; the installer is started by its real path even when its folder has become a junction; nothing is offered while the record of seen releases cannot be saved; automatic checks at most once a day, the *Updates* toggles, no contact with GitHub when updates are off or the build has no key, and only the settings page can use the updater. `AutoUpdateTest`, the default: an update downloads by itself and the toolbar's *Relaunch to update* (clicked) hands it to the installer, keeps the normal windows' tabs once (also with *Open a new tab*) and the next start restores them and opens What's new; closing TorFox with an update waiting starts the installer silently with no relaunch; in both, the fake installer records that TorFox held the setup's mutex when it started it, was still waiting 1.5 s later and exited once the installer set `TorFoxUpdateStarted` (`tests/e2e/helpers/fake_installer.c`), and the restarted TorFox reports that the update did not install (the fake installs nothing); not while Windows ends the session; with a private window open TorFox asks first (*Not now* changes nothing); nothing is kept while history is cleared on exit, and then the prompt does not promise the tabs back. `UpdateBeingInstalledTest`: a TorFox started while the setup's mutex is held exits before CEF starts and records the request in `HKCU\Software\TorFox` (the value is restored afterwards). Windows only |
| `test_site_isolation.py` | Stage 9: a frame of another site runs in its own renderer process (an out-of-process frame, seen as a DevTools target), a frame of the same site does not (the control), in normal and private windows |
| `test_whats_new.py` | What's new opens once after an update (the record names an older version), with the notes built into TorFox (`docs/release-notes/<version>.txt`) as text; never for a new profile, the same or a newer version, or a damaged record; it says the update was checked against the signing key only when `update-handoff` names this version, and a record naming a newer version shows a notice once that the update did not install; only its page can ask for the notes, and it can ask for nothing else |
| `test_memory.py` | Settings > Memory, checked against Windows' own process list (`Win32_Process`): every process counted is TorFox's or a descendant, tab rows are renderer processes, the GPU and network processes are helpers, nothing is counted twice or left out, the browser figure fits its committed private memory; private tabs are never named; the section updates while shown and pauses while hidden; only Settings can ask. Tor's row: `test_tor.TorProcessTest` |
| `test_js_optimizer.py` | Settings > Privacy & Security > JavaScript optimizer, read from each tab's renderer command line (`--js-flags=--disable-optimizing-compilers`): off in Tor windows by default, on in normal and private windows; *Off everywhere* and *On*; a change applies to pages opened after it |
| `test_suggestions.py` | Stage 9: address-bar suggestions. Typing lists matching bookmarks and history entries under the address bar; the arrow keys and Enter, or a click, open one in the active tab; Escape, leaving the address bar and switching tabs hide the list; the list's page can only say which row was clicked, cannot be opened in a tab, and closes with its window. Tor windows list bookmarks only (`test_tor.TorSuggestTest`) |
| `test_downloads.py` | The download bypass matrix (11 vectors × Disable / Ask-cancel / Ask-accept / Allow), prompts held and cancelled by closing the tab or turning downloads off, staging folder, partial names, safe and unique names, Mark of the Web (private downloads keep it but record no address), File System Access pickers absent, the downloads page and private-window separation, names that fit the path limit and are reserved while waiting, failed, restarted and stalled downloads, `OnBeforeDownload` on its own (`InnerLayerTest`), Chromium's download records erased after a quit and a crash (`DownloadRecordsTest`), the real Downloads folder untouched |

`TORFOX_QUIET_SECONDS` sets the idle time (the default is 20; CI uses 60).

Known gaps are written as `expectedFailure` tests, so a fix shows up as an unexpected success. See
[`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md), [`STAGE2_FINDINGS.md`](STAGE2_FINDINGS.md),
[`STAGE3_FINDINGS.md`](STAGE3_FINDINGS.md), [`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md),
[`STAGE5_FINDINGS.md`](STAGE5_FINDINGS.md), [`STAGE6_FINDINGS.md`](STAGE6_FINDINGS.md) and
[`STAGE7_FINDINGS.md`](STAGE7_FINDINGS.md).

The tests open real windows. Don't type while they run: keystrokes can land in a test window and make it
browse for real (this happened once and showed up in the quiet-startup test).

## Release-build platform tests (Windows)
```
python -m unittest discover -v -s tests/platform -p "test_*_win.py"
```
These run the **release** build in `out/dev` (or `TORFOX_RELEASE_EXE`) the way a user would. They check:
- the sandbox level of every process type;
- that hostile command-line switches change nothing;
- that no log file is created, not even an empty one (Chromium's, CEF's or TorFox's test log), even with
  logging switches, and nothing appears next to `TorFox.exe`.

They use the per-user data folder `%LOCALAPPDATA%\TorFox`. Your own folder is set aside (renamed to
`TorFox.platform-test-aside`) while each test module runs and put back afterwards, and the run fails
if any file in it changed (`tests/platform/real_profile.py`). A run that was killed leaves it aside; the
next run puts it back first, keeping whatever it finds in its place as `TorFox.platform-test-leftover-*`.
They are skipped while another TorFox is running, because a second launch would only open a window in the running one.

`test_installer_win.py` installs the Windows setup and uninstalls it again. Build the setups first:
```
python scripts/package_windows.py --test-peruser --compression none
python scripts/package_windows.py --test-peruser --compression none --next-patch
python scripts/package_windows.py --compression none                 # the real setup
python scripts/package_windows.py --compression none --next-patch
```
`--next-patch` builds the same files labelled with the next patch version, for the update test. Both
setups install for the current user only, and neither test needs administrator rights.
- **Test build** (`PerUserInstallerTest`): the setup under its own name and uninstall entry, installed into
  a temporary folder. It never touches a real installation.
- **Real setup** (`ReleaseSetupInstallerTest`): installed where it installs by default,
  `%LOCALAPPDATA%\Programs\TorFox`. It is skipped if TorFox is already installed there for this user
  (or its Start-menu shortcut exists), so a real installation is never touched.

Both start the installed TorFox, a release build, with the data folder `%LOCALAPPDATA%\TorFox`; your
own folder is set aside meanwhile, as above.

Each variant checks Setup's log (non-administrative install mode, under `HKEY_CURRENT_USER`), the
installed files against the setup's allowlist, the shortcut, the uninstall entry,
that the installed TorFox runs sandboxed, the update path (the setup is started with the arguments TorFox's
updater uses: it waits for TorFox to close, installs and starts TorFox again, or gives up after a minute
without changing anything), the update after TorFox closed (`/TORFOXNORELAUNCH`: it installs and does not
start TorFox), a TorFox opened while an update installs (it exits at once, and the setup starts TorFox when
it is done) and that the setup and uninstaller refuse while TorFox is open.

## Release tooling
```
python -m unittest discover -v -s tests/tools
```
Checks `scripts/release.py`, which the release workflow uses: the manifest, the check of a signed release
(made with the official minisign when it has been fetched), that preflight fails without the release key or
notes, and that in `release.yml` only the sign job (environment `release`) reads the signing secret, the
tag's commit must be on `main` and every action is pinned by commit. The workflow itself has never run.
See [`RELEASING.md`](RELEASING.md).

## Mutation check
```
python tests/e2e/run_mutations.py            # all mutations
python tests/e2e/run_mutations.py <name> ... # some
```
The script removes one protection from the source at a time, rebuilds, and runs the checks that are meant to
catch it. A mutation counts as **detected** only if every listed check fails. If any check still passes, it
isn't really testing that protection, and the script fails.

Notes:
- Sources are always restored, and both trees are rebuilt from the clean sources at the end.
- A skipped check counts as an error, not a detection.
- Installer mutations (`installer:` checks) rebuild the release build and the per-user test setups, then run
  `PerUserInstallerTest` tests.
- `TORFOX_CMAKE` can point at the CMake to use.

Run the mutation check after changing a security test or the code it covers. A new protection should get
a mutation of its own.

## Continuous integration
`.github/workflows/ci.yml`:
- **Windows:** runs everything above, except mutations, on every push to `main` and on pull requests,
  including both installer variants.
- **macOS:** builds the arm64 and x86_64 bundles, runs the unit tests, checks the bundle layout and makes,
  mounts and starts the DMG, on pull requests and manual runs only. The macOS E2E and sandbox tests haven't
  been written yet.
- CI has not run yet: nothing has been pushed to GitHub.

`.github/workflows/release.yml` makes releases ([`RELEASING.md`](RELEASING.md)).
