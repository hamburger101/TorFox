# Stage 7 findings: signed updater

Stage 7 adds updates from the official repository's GitHub Releases: each release is checked against a
minisign signature and then by version, size and SHA-256. Settings → Updates and its three settings are
now active. Results:

- **Unit tests:** 177 tests: 176 pass, 1 skipped (`ClearData.NeverFollowsLinks`, which needs permission to create symbolic links; unchanged since Stage 4).
- **End-to-end tests:** 229 tests (test build, no network): all pass, 3 skipped (the real-Tor tests, run separately). Two earlier full runs each had one failure in an older test: the tab-drag test stalled because its window was covered by another (§3.5), and the session-restore test read the list of windows twice and could see two different lists. Both tests were fixed; the full run after that passed.
- **Mutation check:** pending: it runs on this commit, and its results follow in the next one.

## 1. What was built
- **Signature check** (`src/core/update/minisign.cc`). minisign's prehashed format (Ed25519 over
  BLAKE2b-512), checked with libsodium 1.0.22: key ID, the signature over the file, and the global
  signature over the trusted comment. Legacy (non-prehashed) signatures are refused. The test vectors in
  `tests/unit/data/update` were made by the official minisign 0.12 tool.
- **Release rules** (`src/core/update/release.cc`). The manifest format, the signed comment
  `torfox-release version:<v> timestamp:<t>`, version order, anti-replay (`newestSeen`), file names, sizes
  and hashes, the GitHub host allowlist, and the reading of GitHub's *latest release* answer. All of it is
  pure C++ with no CEF, so the unit tests cover it directly.
- **Update service** (`src/browser/app/update_service.cc`). Checks (by hand, or automatically at most once a
  day), downloads with a size limit and a running SHA-256, re-hashes before install, and hands the file to
  the platform layer after `CefShutdown`. Every request and every redirect is checked against the
  allowlist by TorFox itself (§3.2). State is kept in `update.json` in the data folder.
- **Settings → Updates.** A status card (installed version, state, release notes as plain text, progress,
  *Check now*, *Download*, *Install and restart*). The three settings (*Check for updates*, *Check
  automatically*, *Tell me when an update is available*) are active. An automatic check that finds an
  update shows a notice in a normal window.
- **Platform hand-off.** Windows: the installer is opened and kept open (no write sharing) while its hash
  is checked again, then started with `/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE`. macOS: the DMG
  is hashed again and opened.
- **Dependencies.** libsodium 1.0.22 (official prebuilt static MSVC libraries on Windows; built from the
  official source on macOS) and the official minisign 0.12 tool. Both are checked against the upstream
  minisign signature (the key published in libsodium's documentation and minisign's README) before their
  SHA-256 is pinned in `deps.lock.json`. `scripts/fetch_deps.py` now checks those signatures itself.
- **Test signer.** `scripts/minisign.py` (pure Python, RFC 8032 Ed25519) signs test releases with a new
  key for each test run. It is not constant-time and is never used with a real key. As a manual cross-check,
  the official minisign 0.12 accepted 5 of 5 signatures made by it and refused all 5 after one byte of the
  file was changed.

## 2. Changes from the plan
- **No operating-system signature check.** The plan had a fourth check, the OS code signature, once a signer
  identity exists. TorFox will not be distributed through app stores and has no Authenticode or Developer
  ID certificate, so there is no identity to check. Updates rest on the minisign signature alone. This is
  stated in [`UPDATING.md`](UPDATING.md) and [`SECURITY.md`](SECURITY.md).
- **macOS opens the DMG instead of swapping the app.** The plan had mount → verify the app's signature →
  staging copy → atomic swap → relaunch. Without a Developer ID there is no app signature to verify after
  mounting. There is also no Mac here to test a swap of a running app bundle, and that would be a
  security-critical path that no test has run. So TorFox checks the DMG and opens it, and the user copies the
  app. This can be revisited when a Mac and a signing identity are available.
- **The release key is not made yet.** `config/release.json` has the repository but no public key, so
  builds from this repository never look for updates. The maintainer makes the key (see
  [`RELEASING.md`](RELEASING.md)). The secret key exists only as a GitHub Actions secret, and the release
  workflow fails if it is missing.

## 3. Findings
### 3.1 CEF hands each callback a new `CefURLRequest` wrapper
The first version checked that a `CefURLRequestClient` callback came from the request it had started by
comparing the `CefURLRequest` pointer. CEF's C-to-C++ layer (`CToCpp`) wraps the same request in a new
object for each callback, so the comparison always failed. Every answer was ignored and the check stayed
*Checking* for ever. The client now relies on its own `finished_` flag and its own request reference.
`test_a_signed_newer_release_is_checked_downloaded_and_installed_after_exit` found this.

### 3.2 Redirects are checked by TorFox, not followed by Chromium
`UR_FLAG_STOP_ON_REDIRECT` delivers a 3xx response with its `Location` header to `OnRequestComplete`, and
Chromium does not follow it. TorFox checks `Location` against the allowlist (and counts redirects, at
most 5) before it starts a new request. Without the flag, the allowlist could only be checked after a
redirect had already been followed. `test_downloads_follow_redirects_only_to_allowed_hosts` checks both an
allowed and a refused redirect, and `test_release_files_of_another_repository_are_not_used` checks that
an allowed host is not enough when the path is not the repository's.

### 3.3 minisign writes CRLF on Windows
The official minisign on Windows writes signature files with CRLF line endings. TorFox accepts LF and CRLF.
`Minisign.MalformedSignatureFilesAreRefused` checks that the same signature verifies with either ending,
and that missing, reordered, extra, cut and oversized lines are refused. The vectors are kept with LF
endings, and that test fails if they are not, so Git's line-ending conversion cannot change what is tested.

### 3.4 The updater's context reaches loopback in test builds only by configuration
The update connection uses an off-the-record context that, like private windows, has no resource gate. Its
only limit is the allowlist. In release builds that is GitHub's four https hosts. Test builds accept
`--torfox-test-update-config`, which names the fake GitHub's loopback address instead. That switch is
read only when the compile-time constant `kTestHooks` is true. `test_release_builds_ignore_the_test_update_source`
(`tests/platform/test_release_win.py`) gives it to a release build, pointed at a local server, and checks that
the server is never asked for a release.

### 3.5 Test windows covered by other windows stalled their input
`test_drag_a_tab_to_reorder` failed in the first full run, and in some runs on its own. The test log
showed each synthetic mouse event taking about 5 seconds: the test took 56 s instead of 2. Chromium
treats a window that other windows cover as hidden and stops producing frames for it. Input that waits for
a frame (pointer moves are aligned to frames) then waits for a timeout. Whether a test window was covered
depended on what else was on the screen. Test builds now accept Chromium's
`--disable-backgrounding-occluded-windows`, and the harness passes it to every test. Release builds drop it
like every other switch. With it, the test took 1.6 s and passed 3 runs of 3. Earlier full runs were
probably slowed in the same way.

### 3.6 The release-build switch test looked for its decoy in the wrong folder
`test_command_line_cannot_weaken_torfox` passes `--log-net-log` and checks that no file appears. A NetLog
(`x.json`) was found next to the release `TorFox.exe`, left by a mutation run in which that switch was
let through. The test looked for it in its own working folder, where it never appeared. The mutation was
still detected (the test checks more than this one file), but this check could not have seen it. The test now passes an
absolute path and checks that path.

### 3.7 lit's `styleMap` breaks under the strict CSP on first render
The shell's test log had a console error at every start: *Applying inline style violates the following
Content Security Policy directive 'style-src 'self''*. lit's `styleMap` writes its first value as a
`style="…"` attribute, which TorFox's CSP refuses, and switches to the CSSOM only on later updates. A
progress bar (the download progress on the Downloads page and in Settings → Updates, Tor's connection
progress) was therefore drawn without its width until it next changed. A small directive
(`ui/src/style-props.ts`) now always uses the CSSOM, which the CSP allows. The CSP itself is unchanged.
`test_pages_set_no_style_attributes` opens the shell and each internal page and fails on any such console
error. Nothing had tested for it before.

## 4. How it is tested
- **Unit** (`tests/unit/update_test.cc`): the official tool's signatures verify. Other keys, legacy
  signatures, changed data, changed trusted comments and malformed signature files are refused. Public-key
  parsing. Manifest rules (format, product, version, timestamp, comment match, file names, sizes, hashes,
  limits). Version order and anti-replay. Repository names. The host allowlist (ports, user names, percent
  signs, upper case, other schemes). GitHub's answer (drafts, pre-releases, files in other folders). The saved
  state. SHA-256 known answers and files.
- **End-to-end** (`tests/e2e/test_updates.py`, Windows, 15 tests): see [`TESTING.md`](TESTING.md). The full
  flow ends with the fake installer started after TorFox exits, with the expected arguments and the file
  whose hash was checked.
- **Mutation check:** 15 new mutations, each removing one check (§5).

## 5. Mutation check
Pending (see the results above).

## 6. Gaps
- **Real GitHub.** No release has been signed with the official key, so no test has run against GitHub
  itself. The fake GitHub serves the same answers as GitHub's API, but redirects to GitHub's real download
  hosts have not been exercised.
- **macOS.** The macOS hand-off (re-hash, open the DMG) and the libsodium source build have only been
  written here. They are first built, and the unit tests first run, by the macOS CI job. No test runs the
  macOS hand-off end to end.
- **Windows installer.** The hand-off is tested with a fake installer. The real setup's `/TORFOXUPDATE`
  handling is tested on its own in Stage 8 ([`STAGE8_FINDINGS.md`](STAGE8_FINDINGS.md)); no single test
  runs both.
