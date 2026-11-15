# Installing TorFox

Download from the repository's **Releases** page on GitHub only (`github.com/hamburger101/TorFox/releases`).
Each release lists the SHA-256 of every file in its signed `manifest.json`. TorFox has no other download
site and no mirrors.

TorFox is not distributed through the Microsoft Store or the Mac App Store, and its files are **not signed
with a Microsoft (Authenticode) or Apple (Developer ID) certificate**. Windows and macOS therefore warn
the first time you run it. Updates installed by TorFox itself are checked against TorFox's own release
signature (see [`UPDATING.md`](UPDATING.md)), which is independent of those certificates.

## Windows 10 / 11 (x64)
1. Download `TorFox-<version>-win-x64-setup.exe`.
2. Optionally check it: in PowerShell, `Get-FileHash .\TorFox-<version>-win-x64-setup.exe` must print the
   `sha256` listed for `windows-x64` in that release's `manifest.json`.
3. Run it. Windows SmartScreen says *Windows protected your PC* because the file has no certificate:
   choose **More info → Run anyway**. Then accept the User Account Control prompt: TorFox installs for all
   users in `C:\Program Files\TorFox`, where ordinary programs cannot change its files.
4. The setup adds TorFox to the Start menu (and, if ticked, the desktop) and an entry in
   **Settings → Apps → Installed apps**.

Requirements: Windows 10 or 11, 64-bit x64. (Windows on Arm is not built or tested.)

**Uninstall:** Settings → Apps → Installed apps → TorFox → Uninstall. The uninstaller refuses while
TorFox is open. It removes the program files, shortcuts and the uninstall entry. Your profile
(settings, bookmarks, history, Tor state) in `%LOCALAPPDATA%\TorFox` is **kept**, so a reinstall finds it
again. Delete that folder yourself to remove it. Downloaded files stay in your Downloads folder.

**Updates:** TorFox downloads and checks the new setup, then asks you to restart it. The setup waits up
to a minute for TorFox to close, installs (with one UAC prompt) and starts TorFox again. If TorFox is
still open after a minute, the setup stops without changing anything.

## macOS (Apple silicon and Intel)
1. Download the disk image for your Mac: `TorFox-<version>-mac-arm64.dmg` (Apple silicon: M1 and later)
   or `TorFox-<version>-mac-x64.dmg` (Intel).
2. Optionally check it: `shasum -a 256 TorFox-<version>-mac-arm64.dmg` must print the `sha256` in
   that release's `manifest.json`.
3. Open the disk image and drag **TorFox** onto **Applications**.
4. The first time, macOS refuses to open it because it is not from an identified developer (the app is
   only ad-hoc signed):
   - macOS 15 and later: open TorFox once (it is blocked), then go to **System Settings → Privacy &
     Security**, find *"TorFox" was blocked…* and choose **Open Anyway**, then confirm with your password.
   - macOS 14 and earlier: Control-click TorFox in Applications, choose **Open**, then **Open**.

   This is needed once per install and once after each update.

Requirements: macOS 13 Ventura or later.

**Uninstall:** quit TorFox and move it from Applications to the Bin. Your profile in
`~/Library/Application Support/TorFox` is kept. Delete that folder to remove it.

**Updates:** TorFox downloads and checks the new disk image, then opens it. Quit TorFox and drag the new
TorFox onto Applications, replacing the old one, then allow it in Privacy & Security as above.

## Building from source
See the README.

## What is verified
- **Windows:** the setup's contents, shortcuts, uninstall, the installed TorFox running sandboxed, and an
  update that waits for TorFox and restarts it are tested automatically
  (`tests/platform/test_installer_win.py`). These tests pass with a per-user test build of the setup.
  The real per-machine setup needs administrator rights and is tested on CI, which has not run yet.
- **macOS:** CI is set up to make the disk image, mount it, check the app's ad-hoc signature, copy the app
  out and start it. This has not run yet. The Open Anyway step and drag-to-Applications cannot be
  automated and are not tested.

See [`FEATURE_STATUS.md`](FEATURE_STATUS.md).
