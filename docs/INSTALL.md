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
   choose **More info → Run anyway**. TorFox installs for you only, in
   `%LOCALAPPDATA%\Programs\TorFox`. It needs no administrator rights, so there is no User Account Control
   prompt, either now or for updates.
4. The setup adds TorFox to the Start menu (and, if ticked, the desktop) and an entry in
   **Settings → Apps → Installed apps**.

Requirements: Windows 10 or 11, 64-bit x64. (Windows on Arm is not built or tested.)

**Uninstall:** Settings → Apps → Installed apps → TorFox → Uninstall. The uninstaller refuses while
TorFox is open. It removes the program files, shortcuts and the uninstall entry. Your profile
(settings, bookmarks, history, Tor state) in `%LOCALAPPDATA%\TorFox` is **kept**, so a reinstall finds it
again. Delete that folder yourself to remove it. Downloaded files stay in your Downloads folder.

**Updates:** TorFox downloads and checks each new setup by itself, then shows *Relaunch to update* in the
toolbar. Relaunching closes TorFox; the setup waits up to a minute for it to close, installs and starts
TorFox again, with no prompt, and your normal windows' tabs come back. If you close TorFox instead, the
update installs in the background and TorFox stays closed until you open it. If TorFox is still open
after a minute, the setup stops without changing anything. Settings → Updates can turn automatic
downloads off ([`UPDATING.md`](UPDATING.md)).

**Installed an earlier test build for all users?** Setups from before this change installed TorFox in
`C:\Program Files\TorFox`. The new setup does not replace that copy; it installs a second one for you.
Uninstall the old one from **Settings → Apps → Installed apps** (Windows asks for administrator rights this
one last time, because that copy was installed with them). Both copies use the same profile, so nothing is
lost.

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

**Updates:** TorFox downloads and checks the new disk image by itself, then shows *Relaunch to update*.
That opens the image after TorFox quits. Drag the new TorFox onto Applications, replacing the old one,
then allow it in Privacy & Security as above.

## Building from source
See the README.

## What is verified
- **Windows:** the setup's contents, shortcuts, uninstall, the installed TorFox running sandboxed, and an
  update that waits for TorFox and restarts it are tested automatically
  (`tests/platform/test_installer_win.py`), with the real setup in its default folder and with a test
  build of it in a temporary folder. Both run without administrator rights. A test also checks the
  setup's own log: it installs in non-administrative mode, under `HKEY_CURRENT_USER`.
- **macOS:** CI is set up to make the disk image, mount it, check the app's ad-hoc signature, copy the app
  out and start it. This has not run yet. The Open Anyway step and drag-to-Applications cannot be
  automated and are not tested.

See [`FEATURE_STATUS.md`](FEATURE_STATUS.md).
