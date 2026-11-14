# Security

> TorFox is in early development, and no release exists yet. Don't use it where security or anonymity
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
| `.onion` names leaking to DNS from regular tabs | Navigation and resource guards, plus a resolver rule that maps `*.onion` to "not found" | Verified on Windows |
| Unrequested connections (telemetry, background services) | No crash reporter or metrics; Chromium background services turned off; no proxy auto-discovery unless the user turns on the system's proxy settings | Verified for an idle session on Windows |
| Network observers linking `.onion` browsing to you | Bundled Tor, an isolated Tor context, fail closed | Not built yet (Stage 5) |
| Tampered or downgraded updates | Ed25519 (minisign) signed manifest, SHA-256 per asset, anti-rollback | Not built yet (Stage 7) |
| Trackers, third-party cookies, HTTP downgrade | EasyPrivacy domain list, third-party cookie blocking, HTTPS-first with a question before http | Verified on Windows; tracker WebSockets and http form posts are not covered ([`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md) §6) |
| Sites using the camera, microphone or clipboard, or opening other apps | Denied by default; *Ask* asks every time; only checked `mailto:`/`tel:` links reach the OS, after a click and a question | Verified on Windows, except the final hand-off to the mail or phone app |
| Browsing traces left on disk | Clear on exit (after shutdown and again at the next start); Chromium's per-site scores always erased | Verified on Windows |

## What TorFox does not defend against
- **Malware on your machine.** Anything running under your account (or as administrator) can read
  TorFox's profile and change its files.
- **Browser fingerprinting.** TorFox is **not** Tor Browser. Sites can fingerprint it, and in Tor tabs it
  doesn't give Tor Browser's anonymity. Tor circuits are isolated per destination, not per first-party site.
- **Bugs in Chromium or CEF.** These are fixed only by updating to a newer CEF build. TorFox is only as current as the CEF it ships.
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
