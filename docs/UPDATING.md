# Updates

TorFox can update itself from the GitHub Releases of its official repository. This page describes what it
checks, when it connects, and what happens on Windows and macOS. Maintainers: see
[`RELEASING.md`](RELEASING.md) for making a release.

## When a build can update
A build looks for updates only when `config/release.json` names a repository **and** contains the release
public key. Both are compiled in. The repository's own `config/release.json` has the repository
(`hamburger101/TorFox`) but **no public key** until the maintainer makes one (see `RELEASING.md`), so
current builds never contact GitHub for updates. *Settings → Updates* says so ("not configured").

## What the user controls (Settings → Updates)
| Setting | Default | Effect |
|---|---|---|
| Check for updates | on | Off: TorFox never contacts GitHub for updates, not even when *Check now* is pressed |
| Check automatically | on | Off: TorFox checks only when *Check now* is pressed |
| Download updates automatically | on | See below. Off: nothing is downloaded until the user presses *Download*, and nothing is installed until they press *Relaunch to update* |
| Tell me when an update is available | on | With automatic updates off: off means an automatic check that finds an update shows no notice (the Updates page still shows it) |

Automatic checks happen at most once a day. The first one is two minutes after TorFox starts, if the last
check was more than a day ago.

### Automatic updates (the default), like Chrome's
1. A check that finds a release which passes every check below **downloads it by itself**, and checks the
   file. Turning the setting on while an update is waiting downloads it at once.
2. The toolbar then shows **Relaunch to update**. Settings → Updates shows the same button.
3. *Relaunch to update* closes TorFox, installs the update and starts TorFox again. The tabs of normal
   windows come back once, even if *On startup* is *Open a new tab*. Private and Tor windows never come
   back, so if any is open TorFox asks first ("Your private or Tor window will close"; *Not now* changes
   nothing). While *Clear when TorFox closes → History* is on, no tabs are kept for the relaunch either.
4. If the user never relaunches, the update is installed **when TorFox is closed** (Windows), without a
   window and without starting TorFox again. It is not installed while Windows is shutting down or
   restarting, because the installer could be cut off; it waits for the next time TorFox closes. On macOS
   nothing is installed on close (see *Installing*).
5. After the update, the first normal window opens **What's new** (`torfox://whatsnew/`) once, in front
   of the restored tabs. If the update did not install, TorFox says so when it next starts ("TorFox could
   not install the update to version …") and stays on its version.

## What is checked
Every release must pass all of these, in this order. Any failure stops the update, says why, and leaves
the installed TorFox unchanged.
1. **Where it comes from.** Only `https://api.github.com/repos/<owner>/<repo>/releases/latest` is asked, and
   only these hosts are contacted, for every request and every redirect: `api.github.com`, `github.com`,
   `objects.githubusercontent.com` and `release-assets.githubusercontent.com`. Any other host, scheme, user
   name or port ends the check. At most 5 redirects are followed. Drafts and pre-releases are ignored, and the
   release's files must be in that repository's `releases/download/<tag>/` folder. There is no mirror.
2. **The signature.** The release must have `manifest.json` and `manifest.json.minisig`. The signature is a
   [minisign](https://jedisct1.github.io/minisign/) signature (Ed25519 over BLAKE2b-512, "prehashed") and
   must be made by the key compiled into TorFox. Legacy (non-prehashed) signatures are refused. The signed
   *trusted comment* must be exactly `torfox-release version:<version> timestamp:<unix time>` and match the
   manifest. TorFox checks signatures with libsodium.
3. **The version.** The release must be strictly newer than the installed version, and not older than the
   newest release TorFox has already seen (so an old, validly signed release cannot be replayed to bring
   back a fixed bug).
4. **The file.** The manifest names the file for this computer (`windows-x64`, `macos-arm64` or
   `macos-x64`), its size and its SHA-256. The file name must be `TorFox-<version>-…` and end in `.exe` or
   `.dmg`. The download is limited to that size, hashed while it downloads, and deleted unless both size and
   SHA-256 match.
5. **Before installing**, the file is hashed again. On Windows it is then kept open so that it cannot be
   changed until the installer has started.

The update connection uses its own off-the-record browser context: no cookies, no cache, nothing shared
with any window. It uses the same network settings as normal windows (not Tor). GitHub sees the request
and the computer's IP address. The request carries `User-Agent: TorFox-Updater`, no cookies and no
credentials. TorFox sends nothing else about the computer or its settings.

Downloads are kept in `Updates` in TorFox's data folder until they are installed, and that folder is
emptied each time TorFox starts.

## Installing
Release notes come from the signed manifest and are shown as plain text in Settings → Updates.

**Windows.** *Relaunch to update* closes all windows. After TorFox has shut down, it starts the checked
installer (`/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE`), which waits for TorFox to exit, installs
and starts TorFox again. When TorFox is closed with an update waiting, the installer is started with
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE /TORFOXNORELAUNCH` instead: no progress window,
and TorFox is not started afterwards. TorFox is installed for the current user only, so the installer needs
no administrator rights and Windows shows no prompt.

While an update is being installed, the installer holds a named mutex (`TorFoxUpdateInstalling`). A TorFox
opened during that time does not start, because it would hold files the installer is replacing. It records
a request instead (`HKEY_CURRENT_USER\Software\TorFox`, value `RelaunchAfterUpdate`) and exits. The
installer starts TorFox when it has finished, and deletes the value. A normal start also deletes it.
The installer's behaviour is described in [`INSTALL.md`](INSTALL.md).

There is no moment between TorFox and the installer when a TorFox could start normally. TorFox takes the
same mutex as soon as it knows it will hand an update over, before Chromium shuts down. It keeps the mutex
until the installer signals (the event `TorFoxUpdateStarted`) that it holds the mutex too, or until the
installer exits, or for 20 seconds at most. A TorFox opened in that time behaves as during the install. If
handing over fails, TorFox lets go of the mutex before it shows the error. A TorFox started just as the
installer finishes checks the mutex again after it has recorded its request. If the installer let go in
between, and so may have missed the request, it starts after all.

If the installer stops without installing (for example, because TorFox did not close within a minute),
it still starts TorFox after *Relaunch to update*. Just before it starts the installer, TorFox writes the
version it hands over to `update-handoff` in its data folder. The next start reads that file once and
deletes it:
- If it names this version, the update is the one TorFox checked against its signing key, and What's new
  says so.
- If it names a newer version, the update did not install, and TorFox shows a notice.
- Anything else says nothing.

macOS writes no such file, because the user installs the app.

**macOS.** *Relaunch to update* closes all windows. After TorFox has shut down, it opens the checked DMG.
The user then copies TorFox to Applications, replacing the old copy. TorFox does not replace the app itself
(see *Limits*), so it neither restarts nor installs on close.

## What's new
TorFox keeps the version that last ran in `last-version` in its data folder. When that file names an older
version, TorFox has been updated: the first normal window opens `torfox://whatsnew/` once, and the file is
then set to the current version. The page says the update was checked against TorFox's signing key only
when `update-handoff` shows that this version is the update TorFox itself handed to the installer. A
newer version installed by hand, or a newer development build, also opens What's new, but without that
sentence. A new profile, the same version, a newer one (TorFox was downgraded) or
a file that is not a version opens nothing. The notes on that page are built into TorFox from
`docs/release-notes/<version>.txt` (at most 16 KiB of UTF-8 text) and shown as text, never as markup; they
are not fetched from anywhere. The page can ask TorFox for nothing but those notes and the theme.

## Limits
- The builds are not code-signed (no Authenticode certificate, no Apple Developer ID), so neither Windows
  nor macOS can confirm the publisher. The update's integrity rests on TorFox's own minisign signature
  and on the secrecy of the signing key, which exists only as a GitHub Actions environment secret that
  one job of the release workflow can read (see [`RELEASING.md`](RELEASING.md)).
- Anyone who can see network traffic can see that TorFox contacts GitHub, and roughly when.
- A release signed with the official key is trusted. If that key is stolen, an attacker could sign an
  update; a stolen key must be replaced with a new TorFox release that carries the new public key.
- On macOS the user copies the app by hand. The DMG is checked; the copy is not.
- The tests use a fake GitHub on this computer and a fake installer (see [`TESTING.md`](TESTING.md)). Updating
  from the real GitHub cannot be tested until a release has been signed with the official key.
- Skipping the install while Windows shuts down uses `GetSystemMetrics(SM_SHUTTINGDOWN)`. The tests reach
  that branch with a test switch; a real Windows shutdown is not tested.
- Any program running as the same user can create the `TorFoxUpdateInstalling` mutex. While it holds it,
  TorFox does not start. Such a program could also simply delete TorFox, so this gives it nothing new.
- The mutex, the event and the `RelaunchAfterUpdate` value have fixed names, shared by every copy of
  TorFox the user runs. A development build, or the per-user test setup from `scripts/package_windows.py`,
  also waits while a real update installs, and uninstalling either deletes the empty
  `HKEY_CURRENT_USER\Software\TorFox` key.
- A link opened from another app while an update installs is not opened: that TorFox exits before it reads
  its command line, and the installer starts TorFox without it.
- The second check of the mutex at start-up, for a TorFox started just as the installer finishes, closes
  a race that lasts microseconds. No test reaches it.
