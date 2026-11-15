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
| Tell me when an update is available | on | Off: an automatic check that finds an update shows no notice (the Updates page still shows it) |

Automatic checks happen at most once a day. The first one is two minutes after TorFox starts, if the last
check was more than a day ago. Nothing is downloaded or installed without the user pressing *Download* and
then *Install and restart*.

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
Release notes come from the signed manifest and are shown as plain text before installing.

**Windows.** *Install and restart* closes all windows. After TorFox has shut down, it starts the checked
installer (`/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE`). TorFox is installed for all users in
Program Files, so Windows asks for administrator permission (UAC). If the user declines, nothing changes.
The installer's behaviour is described in [`INSTALL.md`](INSTALL.md).

**macOS.** *Install and restart* closes all windows. After TorFox has shut down, it opens the checked DMG.
The user then copies TorFox to Applications, replacing the old copy. TorFox does not replace the app itself
(see *Limits*).

## Limits
- The builds are not code-signed (no Authenticode certificate, no Apple Developer ID), so neither Windows
  nor macOS can confirm the publisher. The update's integrity rests on TorFox's own minisign signature
  and on the secrecy of the signing key, which exists only as a GitHub Actions secret.
- Anyone who can see network traffic can see that TorFox contacts GitHub, and roughly when.
- A release signed with the official key is trusted. If that key is stolen, an attacker could sign an
  update; a stolen key must be replaced with a new TorFox release that carries the new public key.
- On macOS the user copies the app by hand. The DMG is checked; the copy is not.
- The tests use a fake GitHub on this computer and a fake installer (see [`TESTING.md`](TESTING.md)). Updating
  from the real GitHub cannot be tested until a release has been signed with the official key.
