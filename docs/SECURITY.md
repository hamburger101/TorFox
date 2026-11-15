# Security

> TorFox is pre-release, and no release exists yet. Don't use it where security or anonymity
> matters. [`FEATURE_STATUS.md`](FEATURE_STATUS.md) lists what is verified by test, and on which platform.

## Reporting a vulnerability
Once the repository is public, please report vulnerabilities privately through GitHub's
**Security → Report a vulnerability** form on the repository. Please don't open a public issue for a
vulnerability. Include the TorFox version, the platform, and the steps to reproduce.

## What TorFox defends against
| Threat | Defence | Status |
|---|---|---|
| A malicious web page compromising the machine | Chromium's sandbox for every process type, site-per-process | Sandbox verified on Windows (release build) |
| A web page reaching TorFox's privileged UI or its bridge | Five layers (see [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md) §1); the bridge exists only in internal pages and validates every message | Verified on Windows |
| A shortcut or launcher weakening TorFox with command-line switches | Release builds ignore switches (two independent layers) | Verified on Windows |
| `.onion` names leaking to DNS from regular tabs | Navigation and resource guards, plus resolver rules that map `*.onion` and `*.onion.` (the same name with a trailing dot) to "not found" | Verified on Windows |
| Unrequested connections (telemetry, background services) | No crash reporter or metrics; Chromium background services turned off; no proxy auto-discovery unless the user turns on the system's proxy settings; no DNS probe of google.com after a failed lookup | Verified on Windows for an idle session and for a page whose name does not resolve |
| Network observers seeing what you browse in Tor windows, or local DNS revealing it | Bundled official tor; Tor windows use their own context whose only route is tor's SOCKS port (host names resolved by tor); fail closed in three independent layers; while tor has no port, once it stops, or when *New identity* retires a context, that context's route is a loopback port TorFox itself holds and resets, which no other program can take | Verified on Windows with a scripted tor, and live with the bundled tor ([`STAGE5_FINDINGS.md`](STAGE5_FINDINGS.md)) |
| A Tor window revealing your address or what it visits through WebRTC, local-network requests, other apps or device access | No WebRTC in Tor windows (as in Tor Browser: WebRTC looks names up itself, outside the proxy), and no UDP route but the proxy should it ever be reachable; local addresses, other apps, camera, microphone and clipboard refused in Tor windows | Verified on Windows |
| Tampered, downgraded or replayed updates | A minisign (Ed25519) signature on the release manifest with the key compiled into TorFox; strictly newer versions and no release older than one already seen; size and SHA-256 of the file after download and again before install; GitHub's hosts only, for every redirect ([`UPDATING.md`](UPDATING.md)) | Verified on Windows against a fake GitHub. Builds have no release key yet, so they never look for updates |
| Exploits of the JavaScript engine in Tor windows | Chromium's per-site setting turns off V8's optimizing compilers (TurboFan, Maglev), where most browser exploits strike, for Tor windows by default and optionally everywhere (Settings → Privacy & Security → JavaScript optimizer) | The switch reaching each Tor renderer is verified on Windows; what V8 does with it is Chromium's |
| Trackers, third-party cookies, HTTP downgrade | EasyPrivacy domain list, third-party cookie blocking, HTTPS-first with a question before http | Verified on Windows; tracker WebSockets and http form posts are not covered ([`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md) §6) |
| Sites using the camera, microphone or clipboard, or opening other apps | Denied by default; *Ask* asks every time; only checked `mailto:`/`tel:` links reach the OS, after a click and a question | Verified on Windows, except the final hand-off to the mail or phone app |
| Other users' programs changing TorFox's installed files | Windows: installed for the current user in `%LOCALAPPDATA%\Programs\TorFox`, a folder Windows lets only that user (and administrators) change; the setup and uninstaller refuse while TorFox runs | Both setups tested on Windows without administrator rights; the folder's permissions are Windows' own and not tested by TorFox. Programs running as you can change these files (below). On macOS the app is wherever the user copies it |
| Browsing traces left on disk | Clear on exit (after shutdown and again at the next start); Chromium's per-site scores always erased | Verified on Windows |

## What TorFox does not defend against
- **Malware on your machine.** Anything running under your account (or as administrator) can read
  TorFox's profile and change its files. TorFox installs for you only, without administrator rights, so
  its program files are in your own folder: such a program could replace TorFox or the bundled tor, as it
  could with any per-user install (Chrome's and Firefox's included). Installing for all users would stop
  that, but every install and update would then need administrator rights
  ([`STAGE9_FINDINGS.md`](STAGE9_FINDINGS.md) §2.1).
- **A program running as you keeping TorFox from starting.** While an update installs, TorFox does not
  start (the setup's `TorFoxUpdateInstalling` mutex). Any program running as you can create that mutex
  and so stop TorFox from starting, as it could by deleting it ([`UPDATING.md`](UPDATING.md)).
- **Withheld updates.** A signed release does not expire. Someone who can answer for GitHub to TorFox
  (for example with a certificate your computer trusts) cannot make it install anything unsigned or older,
  but can keep showing it the last release it saw, so it never learns of newer ones.
- **Browser fingerprinting.** TorFox is **not** Tor Browser. Sites can fingerprint it, and Tor windows
  don't give Tor Browser's anonymity. Tor circuits are isolated per destination address, not per
  first-party site, and all Tor windows share one context (cookies included) until *New identity*.
- **Files opened after downloading them in a Tor window.** They can connect to the internet without Tor;
  the download prompt says so.
- **Bugs in Chromium or CEF.** These are fixed only by updating to a newer CEF build. TorFox is only as current as the CEF it ships.
- **A stolen release key.** Whoever holds the minisign secret key can sign an update that TorFox
  installs. It exists only as a secret of the repository's `release` environment, which only the release
  workflow's sign job can read, after a maintainer approves the run (set up as
  [`RELEASING.md`](RELEASING.md) describes; nothing in the repository can check that the settings are
  made). Anyone who can merge to `main` without review or approve that environment can get a release
  signed. TorFox has no code-signing certificates, so neither
  Windows nor macOS confirms the publisher of the installer.
- **Certificate revocation between releases.** TorFox turns off Chromium's component updater, which also
  delivers revocation lists (CRLSets). See [`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md) §3.

## Rules this project follows
- No secrets in the repository. Signing keys live only in CI secrets.
- No operating-system protection is disabled to make a feature work.
- Dependencies are official releases, pinned by SHA-256. Tor bundles are also GPG-verified. A
  dependency that fails verification is never used.
- A failed security check stops what it guards. It is never ignored or turned into a warning.
- A protection is called "verified" only when an automated behaviour test shows it. For the protections it
  covers, the mutation check also shows that the test fails once the protection is removed
  ([`TESTING.md`](TESTING.md)). [`FEATURE_STATUS.md`](FEATURE_STATUS.md) says which protections it covers.
- Updates come only from the official GitHub Releases of the configured repository, and are installed only
  after their signature and hashes verify.

## Architecture and assumptions
See [`ARCHITECTURE.md`](ARCHITECTURE.md), and in particular its *Security assumptions* section.
