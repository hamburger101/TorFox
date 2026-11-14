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
| `--torfox-test-skip-guards=nav[,resource]` | Fault injection: turns off TorFox's navigation or resource guard so the inner layers can be tested on their own |
| `--torfox-test-menu-choice=<file>` | Native menus: instead of showing a menu, log its items (`menu_shown`) and pick the item whose label path (`Folder/Item`) is in the file. The file is deleted each time; no file means the menu is dismissed |

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

## End-to-end tests (Windows)
```
cd tests/e2e
python -m unittest -v test_isolation test_bridge test_internal_pages test_settings test_lifecycle test_stage1_defaults test_quiet_startup test_browsing test_places test_private
```
Each test starts the test build (`out/test/...`, or set `TORFOX_TEST_EXE`) with a fresh temporary profile.
It drives TorFox over DevTools on loopback and serves fixtures from local HTTP servers. No internet access
is needed, and none is used.

| File | Covers |
|---|---|
| `test_isolation.py` | Every web→`torfox://` vector: subresources, workers, frames, navigations, pop-ups, forms, redirects. Also browser-initiated navigations, each guard layer on its own, the bridge being absent from web pages, and internal pages staying internal |
| `test_bridge.py` | Malformed, oversized, duplicate-key and wrong-page bridge messages. A rejected message changes nothing |
| `test_internal_pages.py` | Security headers, strict CSP, eval and inline scripts blocked, Trusted Types, no subframes |
| `test_settings.py` | Persistence across restart, corrupt-file recovery, invalid values falling back to safe defaults |
| `test_lifecycle.py` | Opening and closing tabs and windows, and replacing a browser when its role changes |
| `test_stage1_defaults.py` | Downloads blocked, permissions denied, onion refused and never searched, pop-ups only on a click, DuckDuckGo search, no data in Chrome's `Default` profile |
| `test_quiet_startup.py` | NetLog of an idle session: no connections and no proxy auto-discovery from service contexts. Onion names never reach the resolver |
| `test_browsing.py` | Address bar and DuckDuckGo URLs, onion input, navigation controls, error pages, tabs (new, close, reopen, reorder, drag), keyboard shortcuts, find, zoom, context and app menus |
| `test_places.py` | History recorded only by TorFox (Chromium's `History` stays empty), History page search/remove/clear leaving no bytes behind, bookmarks star/bar/manager and persistence |
| `test_private.py` | Private windows: marking, no history, storage isolated from normal and other private windows, links stay private, nothing left on disk (with a control) |

`TORFOX_QUIET_SECONDS` sets the idle time (the default is 20; CI uses 60).

Known gaps are written as `expectedFailure` tests, so a fix shows up as an unexpected success. See
[`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md) and [`STAGE2_FINDINGS.md`](STAGE2_FINDINGS.md).

The tests open real windows. Don't type while they run: keystrokes can land in a test window and make it
browse for real (this happened once and showed up in the quiet-startup test).

## Release-build platform tests (Windows)
```
python -m unittest discover -v -s tests/platform -p "test_*_win.py"
```
These run the **release** build in `out/dev` (or `TORFOX_RELEASE_EXE`) the way a user would. They check:
- the sandbox level of every process type;
- that hostile command-line switches change nothing.

They use the real `%LOCALAPPDATA%\TorFox` folder and delete it afterwards only if the test created it.
They are skipped while another TorFox is running, because a second launch would only open a window in the running one.

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
- `TORFOX_CMAKE` can point at the CMake to use.

Run the mutation check after changing a security test or the code it covers. A new protection should get
a mutation of its own.

## Continuous integration
`.github/workflows/ci.yml`:
- **Windows:** runs everything above, except mutations, on every push to `main` and on pull requests.
- **macOS:** builds the arm64 and x86_64 bundles, runs the unit tests and checks the bundle layout, on pull
  requests and manual runs only. The macOS E2E and sandbox tests haven't been written yet.
