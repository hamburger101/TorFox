# Stage 10 findings: automatic updates, What's new, Settings > Memory, security polish

Stage 10 makes updates work like Chrome's: an update downloads by itself, the toolbar offers *Relaunch to
update*, TorFox starts again with its tabs and opens *What's new*, and an update that waits when TorFox
closes is installed then. It adds a *Memory* section to Settings and closes two security gaps. Results:

**Results, on Windows 11 with the final code:**

| Suite | Result |
|---|---|
| Unit tests | 194 run: 193 pass, 1 skipped (as before) |
| Platform tests (release build, both setups) | 18 of 18 pass |
| E2E: `test_updates` and `test_whats_new` | 33 of 33 pass |
| UI typecheck and build | Pass |
| Installer script | Compiles |

- **The rest of the E2E suite** passed as a whole (272 tests, 3 live-Tor tests skipped) before the review
  fixes in §4, but has not been run again since. Those fixes changed the toolbar button, startup notices
  and start-up.
- **The mutation checks:** all 23 of Stage 10's mutations were run, and the tests caught every one (§5).
  The first run found one that survived; the test was tightened and a setup bug it hid was fixed.
- **macOS** was not built or tested.

## 1. Security changes
### 1.1 Tor windows' placeholder port is now TorFox's own (was a known gap)
While tor has no SOCKS port (it is starting or has stopped) and after *New identity*, the Tor context's
proxy points at a placeholder, so that a request that gets past TorFox's guards fails rather than going
anywhere. Stage 9 listed the gap ([`STAGE9_FINDINGS.md`](STAGE9_FINDINGS.md) §2.2): the placeholder was
port 9 on loopback, which any program can listen on. Such a program would then be sent the host names Tor
windows open (for example by a WebSocket, which TorFox's guards do not see).

The first fix only bound a port of TorFox's own with `SO_EXCLUSIVEADDRUSE`, without listening. The new test
showed that this is not enough on Windows: another program can still bind the same port on every address
(`0.0.0.0`), and as nothing listened on 127.0.0.1 it would have been given the connection. TorFox now
**listens** on its own 127.0.0.1 port for as long as it runs and resets every connection without reading
it (`platform::ReservedLoopbackPort`). The more specific 127.0.0.1 listener is matched before any wildcard
one.

`test_tor.ProxyLayerTest.test_the_placeholder_port_is_torfoxs_own` checks this with the guards switched off.
Binding 127.0.0.1 on the port fails, with and without `SO_REUSEADDR`. A test listener on `0.0.0.0` of the
same port receives nothing, neither from a direct connection nor from a page loaded in the Tor window, and
TorFox's listener sends nothing back. The dead-tor and *New identity* tests now check that the proxy moves
to this port. Two mutations, bound without listening and back to port 9, are both caught (§5).

On macOS the same listener is used (no `SO_REUSEADDR` or `SO_REUSEPORT`). It has never been compiled.

### 1.2 JavaScript optimizer off in Tor windows (new setting)
Most attacks on browsers go through the JavaScript engine's optimizing compilers. Chromium has a per-site
setting that turns them off (Chrome's *V8 optimizer* setting). Where it is off, Chromium starts the
site's renderer with `--js-flags=--disable-optimizing-compilers`: V8 then runs without TurboFan and
Maglev, and WebAssembly still works. TorFox sets it in each web context: *Settings → Privacy & Security →
JavaScript optimizer* is **Off in Tor windows** by default, or *On*, or *Off everywhere*. A change applies
to pages opened from then on, because renderers keep the flags they started with.

`test_js_optimizer.py` finds each tab's renderer process through Settings > Memory's report and reads its
command line from Windows. Tor windows' renderers have the switch, and normal and private windows' do not
(the controls). *Off everywhere* reaches normal and private windows, a change applies to the next page,
and *On* leaves Tor windows without it. What V8 does with the switch is Chromium's and is not measured.

### 1.3 Release builds write no logs (was untested; the test found a file)
Release builds set `LOGSEVERITY_DISABLE`, but no test showed that no log file appears. The new release
platform test starts the release build with `--enable-logging --v=2 --log-level=0` and a page that logs to
the console. After it closes, the profile must hold no `debug.log`, `cef.log`, `chrome_debug.log` or test
log, and nothing new may be next to `TorFox.exe`.

**It failed.** Every release run creates `User Data\chrome_debug.log`, even with no switches at all.
- **What it held:** the file was empty, with nothing written even with `--v=2` and a page logging errors.
  The copy in a profile used since Stage 8 was empty too.
- **Why it appeared:** CEF builds Chromium to log to a file by default. The severity setting stops anything
  reaching the file, but not Chromium opening it.
- **Fix:** release builds now also force `--disable-logging` (`cmdline::Apply`), and the file is no longer
  created. A user's `--enable-logging` cannot undo it (`CmdlinePolicy.ReleaseBuildsOpenNoLogFile`).
- **Mutation check:** a mutation that removes the switch is caught by both tests (§5).

## 2. Automatic updates
How it works for the user: [`UPDATING.md`](UPDATING.md). What is new in the code:
- **`updates.autoInstall`** (default on). An `available` release is downloaded at once. Turning the setting
  on downloads a waiting one. Off keeps the old behaviour: nothing is downloaded or installed unasked.
- **Relaunch to update.** The toolbar shows it while an update is `ready`; the shell page may send
  `update.install` and nothing else of the updater. With private or Tor windows open TorFox asks first,
  because they do not come back. The normal windows' tabs are kept once (`session-relaunch`), even with
  *Open a new tab*, but never while history is cleared on exit.
- **On close (Windows).** A `ready` update is installed after TorFox shuts down, silently and without
  starting TorFox (`/TORFOXNORELAUNCH`). Not after a fatal error, and not while Windows ends the session:
  the installer could be cut off halfway.
- **During the install.** The setup holds the `TorFoxUpdateInstalling` mutex from its start until its files
  are in place. A TorFox started then would hold files the setup replaces, so it stops before CEF starts,
  records the request in `HKCU\Software\TorFox` and exits; the setup starts TorFox when it is done.
- **What's new.** `last-version` records the version that last ran. When it names an older version, the
  first normal window opens `torfox://whatsnew/` once. Its notes are compiled in from
  `docs/release-notes/<version>.txt` (at most 16 KiB, checked as UTF-8, shown as text). The page has two
  bridge commands, `whatsnew.get` and `appearance.subscribe`.

- **Handing over (from the review, §4).** TorFox takes `TorFoxUpdateInstalling` itself before Chromium
  shuts down and keeps it until the setup signals that it holds it too (`TorFoxUpdateStarted`). So no TorFox
  can start normally between the two. TorFox records the version it hands over in `update-handoff`, and the
  next start compares it with its own version:
  - What's new says the update was checked against the signing key only when the two match.
  - When the record names a newer version, the update did not install, and TorFox says so.
  - After a failed *Relaunch to update*, the setup still starts TorFox.

Every check an update passes is unchanged ([`UPDATING.md`](UPDATING.md) *What is checked*): the signature,
the version, the file's size and SHA-256 after download and again before install, and the installer started
by its real path while held open. Automatic updates only remove the clicks.

## 3. Settings > Memory
A new section shows how much memory TorFox uses: the total, each tab (tabs that share a process are one
row), the browser process, the toolbars and menus, Chromium's helpers, and tor with its bridges. It asks
every 2 seconds while it is shown and the tab is visible, and at once when the tab is shown again.
- The process list comes from the operating system. On Windows it is the private working set, Task
  Manager's *Memory* column. On macOS (never compiled) it is the physical footprint, Activity Monitor's.
- TorFox counts its own process and its descendants that run from its own folder or the tor folder. A
  child counts only if it started after its parent, which rules out reused process IDs.
- Tabs: each renderer tells the browser its process ID. A renderer that lies can only mislabel TorFox's own
  helpers, never count another process. Private and Tor tabs are shown as *Private tab* and *Tor tab*: no
  title, no address, because Settings can be open in any window.

`test_memory.py` compares the report with Windows' own process list (`Win32_Process`).

## 4. Review
A separate reviewer read all of Stage 10's changes, without building or running them. It found no
high-severity problem. Its six findings and what was done:

| # | Finding | Severity | Done |
|---|---|---|---|
| 1 | Between TorFox's shutdown and the setup taking `TorFoxUpdateInstalling` (more than a second: Chromium's shutdown, clearing data, hashing the installer, the setup unpacking itself), nothing held the mutex. A TorFox opened then started normally, and the setup gave up after a minute without a word. | Medium | **Fixed.** TorFox holds the mutex from before Chromium shuts down until the setup signals `TorFoxUpdateStarted` (bounded at 20 s), and lets go of it before showing a hand-over error. Tested: the fake installer records that the mutex existed when it started, that TorFox was still waiting 1.5 s later, and that TorFox exited once signalled (`test_an_update_downloads_by_itself_…`, `test_an_update_waiting_when_torfox_closes_…`); two mutations, both caught. |
| 2 | A start checked the mutex, then wrote its request. In between, the setup could finish (it lets go, then reads), so the start was lost. | Low | **Fixed** by a second check after writing: if the mutex has gone, TorFox starts after all. Not tested: the window is microseconds wide. |
| 3 | What's new said every update "was verified with TorFox's signing key", including a newer setup run by hand. | Low | **Fixed.** The sentence appears only when `update-handoff` names this version (`update::CheckHandoff`, `WhatsNew.HandoffRecordTellsWhetherTheUpdateInstalled`, `test_an_update_torfox_installed_itself_is_said_to_be_checked`); a record naming a newer version shows a notice that the update did not install (`test_an_update_that_did_not_install_is_reported_once`). |
| 4 | The relaunch prompt promised the normal windows back even while history is cleared on exit, and said "TorFox restarts" on macOS, where it only opens the disk image. | Low | **Fixed.** The prompt, the toolbar button (*Update TorFox* on macOS) and Settings say what happens (`test_nothing_is_kept_for_a_relaunch_while_history_is_cleared_on_exit` checks the prompt). The macOS wording is not tested. |
| 5 | A *Relaunch to update* whose setup failed left TorFox closed, with no message. | Low | **Fixed.** The setup's `DeinitializeSetup` starts TorFox whenever it should have, and TorFox then shows that the update did not install (finding 3). It does not start one while TorFox is still running (§5). The failing setup itself is not tested: making it fail takes a minute (`UpdateWaitSeconds`). |
| 6 | The mutex and registry value have fixed names shared by every copy (the per-user test setup, development builds), and a link opened during an install is dropped. | Low | **Documented** in [`UPDATING.md`](UPDATING.md) *Limits*; not changed. |

The reviewer also checked and found sound:
- The download origins are unchanged.
- Install-on-exit hashes the file again.
- The new bridge commands are allowed only on their own pages.
- The memory report never names private or Tor tabs.
- The placeholder listener is loopback-only, not inherited, and resets every connection.
- The JavaScript-optimizer setting fails closed.
- The relaunch session respects clear-on-exit.

The platform tests found two more problems:
- **The release-build log file** (§1.3).
- **An installer test that closed TorFox too early.** It asked a TorFox the setup had just started to
  close before that TorFox had a window. The test now asks again until TorFox has gone.

## 5. Mutation check
Earlier stages ran every mutation and recorded that the tests caught each one (**M** in
[`FEATURE_STATUS.md`](FEATURE_STATUS.md)). Stage 10 adds 23 mutations to `tests/e2e/run_mutations.py`, and
all of them are caught:

| Area | Mutations |
|---|---|
| Updates | `update-auto-install-setting-ignored`, `update-installed-while-windows-shuts-down`, `update-relaunch-closes-private-windows-unasked`, `update-relaunch-keeps-tabs-despite-clear-history`, `update-runs-while-being-installed`, `update-handoff-mutex-not-held`, `update-handoff-does-not-wait-for-the-setup`, `update-not-installed-not-reported`, `update-not-restarted` |
| Installer | `installer-no-installing-mutex`, `installer-relaunches-after-install-on-exit`, `installer-starts-torfox-that-never-closed` |
| What's new | `whats-new-after-any-version-change`, `whats-new-always-says-checked` |
| Memory | `memory-names-private-tabs`, `memory-trusts-renderer-pids`, `memory-query-from-the-toolbar` |
| Tor | `tor-placeholder-only-bound`, `tor-placeholder-port-9`, `tor-retired-context-keeps-proxy` |
| JavaScript optimizer | `js-optimizer-on-in-tor-windows`, `js-optimizer-setting-ignored-by-open-windows` |
| Release logging | `cmdline-release-log-file-opened` |
| Clear on exit (older, re-anchored) | `clear-on-exit-skipped` |

Two older mutations had stopped matching the code after Stage 10 edits and were re-anchored first
(`clear-on-exit-skipped`, `update-runs-while-being-installed`); one, `update-installed-while-windows-shuts-down`,
left a variable unused (a build error) and now keeps it.

**One survived the first run: `update-not-restarted`.** It removes the setup's `[Run]` entry that starts
TorFox after *Relaunch to update*, and `test_update_waits_for_torfox_then_installs_and_restarts_it` still
passed, because the fallback added for review finding 5 (`DeinitializeSetup`) started TorFox instead.
Two changes:
- The setup now logs which of its two ways started TorFox, and the test requires the `[Run]` entry. The
  mutation is now caught.
- Looking at the fallback found a bug: when the setup gave up because TorFox never closed, it still
  "started" TorFox, which only opened a second window in the TorFox that was still running. It now
  checks first and logs that it did not. `test_update_gives_up_when_torfox_stays_open` checks the window
  count, and the new mutation `installer-starts-torfox-that-never-closed` is caught.

**The user's own data was touched by the first run.** The installer checks start the installed TorFox,
a release build that uses the real data folder (`%LOCALAPPDATA%\TorFox`). The tests left an existing
folder in place, so three mutation checks ran TorFox with the user's profile: four visits to the tests'
`127.0.0.1` pages were added to its history, and Chromium's own files (preferences, caches) changed as
they do on any start. Settings, bookmarks and saved tabs were not changed. The four history entries were
removed (the database was backed up first, and its integrity check passes). To stop this happening again,
`test_release_win.py` and `test_installer_win.py` now set the user's folder aside for the whole module
and put it back afterwards (`tests/platform/real_profile.py`), and fail if any of its files changed. The
next full platform run restored it byte for byte.

To run the mutations, with no one using the desktop (they start TorFox windows many times):

```
python -W ignore tests/e2e/run_mutations.py <names>
```

Run this in a separate worktree, as [`TESTING.md`](TESTING.md) describes.

## 6. Not verified
- **macOS:** none of the new platform code (`ReservedLoopbackPort`, `TorFoxProcesses` with `libproc`) has
  been compiled. CI builds it only for pull requests and `main`.
- **A real Windows shutdown** with an update waiting: the branch is reached through a test switch.
- **Updating from the real GitHub:** there is no release key yet (`config/release.json` has none), so no
  build looks for updates. Everything above is tested against a fake GitHub with a key made per run.
