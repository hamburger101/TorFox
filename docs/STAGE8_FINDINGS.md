# Stage 8 findings: installer, DMG and release workflow

> **Changed later:** the Windows setup now installs for the current user only, without administrator
> rights ([`STAGE9_FINDINGS.md`](STAGE9_FINDINGS.md) §2.1). What follows describes the per-machine setup
> as Stage 8 built it.

Stage 8 adds the Windows setup, the macOS disk image, and the workflow that builds, signs and publishes
releases. It also connects the Stage 7 updater to the real Windows setup. Results:

- **Installer tests (Windows):** per-user test build 5 of 5 pass. The 5 per-machine tests are skipped here: they need administrator rights, which CI has.
- **Release-build platform tests:** 3 of 3 pass, including the new `test_release_builds_ignore_the_test_update_source`.
- **Release tooling tests:** 5 of 5 pass, with signatures made by the official minisign 0.12.
- **Unit and end-to-end tests:** see [`STAGE7_FINDINGS.md`](STAGE7_FINDINGS.md). Both stages share one build.
- **Mutation check:** 6 of 6 detected, after the test change in §3.6 (§5).

## 1. What was built
- **Windows setup** (`packaging/windows/TorFox.iss`, Inno Setup 7.1.1).
  - Installs for all users in `C:\Program Files\TorFox`. It needs administrator rights, so ordinary programs
    cannot change TorFox's files.
  - Adds a Start-menu shortcut (and a desktop one if ticked) and an uninstall entry. Windows 10 or later,
    x64 only.
  - Setup and uninstaller refuse while TorFox is running. TorFox is seen as running if its
    `TorFoxBrowserRunning` mutex exists, or if any process (TorFox, a helper, tor) runs from the program
    folder (§3.2).
  - Started by the updater with `/TORFOXUPDATE`, the setup waits up to 60 seconds for TorFox to exit. Then
    it installs and starts TorFox again as the user who started it, not elevated. If TorFox stays open, it
    stops with an error and changes nothing.
  - The uninstaller keeps the profile in `%LOCALAPPDATA%\TorFox`.
- **Packaging script** (`scripts/package_windows.py`).
  - Copies exactly the files TorFox needs from a release build: an allowlist of CEF's run-time files, the
    locale packs and the verified Tor Expert Bundle. It checks that tor and lyrebird are present.
  - Files in the build folder that are not on the list are reported and left out. Debug symbols and
    libraries are left out silently.
  - Adds the license texts (`LICENSE`, CEF's `LICENSE.txt` and `CREDITS.html`, `packaging/licenses/`,
    `THIRD_PARTY_NOTICES.txt`).
  - Inno Setup is the official download pinned by size and SHA-256 in `deps.lock.json`. It is installed
    in portable mode under `.deps`: no registry entries and no uninstaller on the build machine.
- **Per-user test setup** (`--test-peruser`). The same script with another name, AppId and uninstall
  entry, installed for the current user. It lets the installer tests run without administrator rights.
  It is never published, and the release workflow builds only the real setup.
- **macOS disk image** (`packaging/macos/make_dmg.sh`, `check_dmg.sh`).
  - Copies the license texts into the app, signs it ad hoc from the inside out, and checks the signature
    with `codesign --verify --deep --strict`.
  - Makes a compressed DMG with an Applications link and verifies it with `hdiutil verify`.
  - The check script mounts the DMG, checks its contents and the signature, copies the app out with
    `ditto` and starts it, waiting for its sandboxed renderer helper.
- **Release workflow** (`.github/workflows/release.yml`) and `scripts/release.py`.
  - A `v*.*.*` tag runs preflight. It stops unless the tag matches the version, the repository and public
    key are configured, the release notes exist and the signing secret is set.
  - Builds and tests on Windows and both macOS architectures, then builds the setup and the two DMGs.
  - Writes the manifest and signs it with the official minisign, which was itself verified when it was
    fetched.
  - Checks the signature with minisign and again with TorFox's rules (`release.py check`: key, comment,
    every file's size and SHA-256).
  - Publishes as a draft and makes the release public only once every file is uploaded.
- **Documentation.** [`INSTALL.md`](INSTALL.md) (including the SmartScreen and Gatekeeper steps for files
  without certificates), [`RELEASING.md`](RELEASING.md) (making the key, the secret, each release) and
  [`release-notes/`](release-notes/README.md).

## 2. Changes from the plan
- **No Authenticode, Developer ID or notarization.** The plan had these steps wired into CI and switched
  off. TorFox will not be distributed through app stores, so none were added. Windows SmartScreen and
  macOS Gatekeeper warn on first install, and [`INSTALL.md`](INSTALL.md) says what to do. The macOS app is
  ad-hoc signed without the hardened runtime. With ad-hoc signatures, the hardened runtime's library
  validation could stop the app from loading its own framework, and no Mac here can test it. Chromium's
  sandbox does not depend on the hardened runtime. The plan had the hardened runtime on.
- **Inno Setup comes from the pinned official download, not winget.** It is checked by SHA-256 like every
  other dependency, and the same version is used locally and on CI.
- **A per-user test setup.** Not in the plan. Without it the installer tests could run only with
  administrator rights, so never locally.
- **No automatic swap on macOS.** See Stage 7 §2: TorFox opens the checked DMG and the user copies the app.

## 3. Findings
### 3.1 The uninstaller runs from the program folder
Inno's uninstaller starts as `unins000.exe` in the program folder, copies itself to a temporary folder and
runs that copy. The first copy keeps running while the second one works. The first version of the
"program folder in use" check therefore saw `unins000.exe` and refused to uninstall, forever in silent
mode. Processes named `unins*` are now left out. `test_install_contents_shortcut_and_uninstall` covers it.

### 3.2 A mutex alone does not show that TorFox is running
`TorFoxBrowserRunning` is created right after start-up, in the session's own namespace. Another user's
TorFox in another session is not visible through it. Nor is a helper or tor that outlives the main process
for a moment. The setup therefore also asks WMI for any process whose executable is in the program folder.
The elevated setup sees every user's processes. The path is escaped for WQL: backslashes, quotes and the
`LIKE` wildcards `[ _ %`. If WMI is not available, the mutex and Inno's Restart Manager check still apply.

### 3.3 Exit codes are what the updater and the tests rely on
In silent mode, an error returned from `PrepareToInstall` ends the setup with a non-zero exit code before
any file is changed. `test_update_gives_up_when_torfox_stays_open` checks that the setup waited, that the
exit code is non-zero, that the installed version is unchanged and that the open TorFox then closes
normally.

### 3.4 TorFox must not restart elevated
The update setup runs elevated. Starting TorFox from it without `runasoriginaluser` would run the browser
with administrator rights. Both `[Run]` entries use `runasoriginaluser`. The update test checks that
TorFox starts again from the program folder, but not that it is not elevated (§6).

### 3.5 Uncompressed setups for tests
LZMA2 at the highest level takes most of the packaging time. The tests check behaviour, not compression,
so CI and the mutation check build them with `--compression none`. The release workflow builds the
compressed setup and runs the release-build tests before it.

### 3.6 Inno's Restart Manager is a second, rougher stop
`CloseApplications=yes` makes Inno ask Windows' Restart Manager which programs use the files it is about
to replace, and try to shut them down. The mutation check found this: with TorFox's own "is TorFox
running" check removed from Setup, `test_setup_and_uninstall_refuse_while_torfox_is_open` still passed.
Setup still failed, but only after it had tried to shut TorFox down. This time TorFox did not close, so
Setup gave up and rolled back. A program that does close would lose its open windows, and Setup would
then install over it. So the test now also reads Setup's log. The refusal must come from TorFox's own
check (`PrepareToInstall failed: TorFox is running`), before the installation starts and before anything
tries to shut TorFox down. The update test checks the same when it gives up.

## 4. How it is tested
- **Installer** (`tests/platform/test_installer_win.py`): see [`TESTING.md`](TESTING.md). Both variants run
  the same tests. The per-user variant ran here. The per-machine variant needs administrator rights and
  is skipped here. It runs on CI.
- **Release tooling** (`tests/tools/test_release.py`): a release signed with the official minisign passes
  the check. The check refuses another key, a changed file, a signed comment that does not match and a
  changed manifest. The manifest needs every platform and no other file, and preflight fails without
  the key.
- **macOS:** `check_dmg.sh` on CI. Never run yet.

## 5. Mutation check
Run on the Stage 7 and 8 commit (`81f980c`) with the per-user test setups. The first run detected 5 of the
6: `installer-ignores-running-torfox` survived, which is how §3.6 was found. With the test reading Setup's
log, the rerun detected all three installer mutations, so all 6 are detected:

- `installer-update-does-not-wait`, `installer-ignores-running-torfox`, `uninstaller-ignores-running-torfox`:
  the waits and refusals while TorFox runs;
- `update-not-restarted`: TorFox is started again after an update;
- `installer-ships-build-files`: the packaging allowlist;
- `installer-keeps-nothing-running-check`: Setup's whole "is TorFox running" check (the mutex and the
  processes running from the program folder).

## 6. Gaps
- **Per-machine setup.** Not run here (no administrator rights in this session). It is tested on CI, which
  has not run yet.
- **macOS DMG.** `make_dmg.sh` and `check_dmg.sh` have never run: there is no Mac here, and CI has not run.
- **Release workflow.** Never run. It needs the release key, which the maintainer makes
  ([`RELEASING.md`](RELEASING.md)), and a public repository.
- **From updater to setup in one test.** The updater is tested to start the setup with the right file and
  arguments, using a fake installer (Stage 7). The real setup is tested with those arguments. No single
  test downloads the real setup through the updater and installs it.
- **No certificates.** SmartScreen and Gatekeeper warn, and users have to override them on first install.
  This is a known limit, not a test gap.
- **TorFox restarted without elevation.** Not testable here: the per-user setup is never elevated, and on
  CI the tests themselves run elevated, so the "original user" is elevated too. It rests on Inno Setup's
  `runasoriginaluser`.
- **Windows on Arm** is not built.
