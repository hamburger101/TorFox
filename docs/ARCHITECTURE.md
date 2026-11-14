# TorFox architecture

This document describes the target architecture and the security assumptions behind it. Evidence for
each engine-level decision is in [`STAGE0_FINDINGS.md`](STAGE0_FINDINGS.md). Implementation status is
tracked only in [`FEATURE_STATUS.md`](FEATURE_STATUS.md).

## Engine
- **CEF 156 (Chromium 156)**, C++ API, official prebuilt binaries pinned by SHA-256 in `deps.lock.json`.
  A weekly CI job proposes CEF bumps; this is how engine security fixes reach users.
- Browsers use CEF **Alloy style** (no Chrome UI), hosted in **CEF Views** windows. Views is cross-platform,
  so one codebase serves Windows and macOS.
- Rejected:
  - Electron: not designed for untrusted content.
  - Qt WebEngine: its proxy is process-global, so Tor can't be isolated per context.
  - A Chromium fork: can't be built on the available hardware.
  - A custom engine: out of scope.

## Processes and privilege
| Component | Process | Privilege |
|---|---|---|
| Browser core: windows, tabs, policy, settings, Tor service, updater | CEF browser process | The only place policy decisions are made |
| TorFox UI (`torfox://…`) | Sandboxed renderer, Internal context | Validated bridge only |
| Web content | Sandboxed renderers, one site per process | No bridge, no `torfox://` |
| Network service | Sandboxed utility, LPAC (Windows) | Network only |
| tor (+ pluggable transports) | Child processes owned by TorFox | Exits with TorFox |

- **Windows:** CEF's `bootstrap.exe` is shipped as `TorFox.exe` and loads `TorFox.dll` (`RunWinMain`).
  Sandbox levels are measured by `tests/platform/win_process_inspect.py`: renderers run Untrusted with
  restricted tokens.
- **macOS:** helper app bundles, with `libcef_sandbox.dylib` initialised in each helper. Hardened runtime;
  `allow-jit` only for the Renderer and GPU helpers.
- **Command-line hardening:** release builds drop every command-line switch that isn't on an allowlist,
  then add TorFox's own switches:
  - `--host-resolver-rules="MAP *.onion ~NOTFOUND"`
  - `--disable-extensions`
  - the network-service sandbox feature
  - background-networking disables, merged into CEF's own feature switches

## Request contexts (isolation boundaries)
| Context | Storage | Network | Notes |
|---|---|---|---|
| Base (global) | Disk, never hosts content | — | Holds the strict baseline prefs |
| Internal | Off-the-record (OTR) | None; http(s) requests are cancelled | Only context with the `torfox` scheme handler |
| Normal | Disk profile | Direct | HTTPS-first, trackers, 3P cookies, pop-ups, permissions. `.onion` is cancelled and offered in a Tor tab |
| Private | Its own OTR profile, one per private window | Direct | No history and no downloads list |
| Tor | Its own OTR profile | SOCKS5 to tor only | See below |

The Tor context:
- **Proxy:** the proxy pref is applied in `OnRequestContextInitialized` and read back before any browser is created.
  It is `fixed_servers`, never DIRECT, with bypass list `<-loopback>`.
- **Traffic:** every request goes through Tor: onion and clearnet subresources alike.
- **Hardening:** `webrtc.ip_handling_policy = disable_non_proxied_udp` and prediction off.
  Camera, mic, location, notifications and external protocols are hard-denied, and history is never recorded.

A tab changes context only by replacing its browser. Popups inherit the opener's context.

## Internal UI (`torfox://`)
The UI is HTML/TypeScript, rendered in a separate privileged browser:
- **Scheme handler:** registered only on the Internal context, with `STANDARD | SECURE | DISPLAY_ISOLATED`.
- **Resources:** served from a compiled-in read-only table (exact-path lookup).
- **Content Security Policy:** `default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:;
  font-src 'self'; connect-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'`.
- **Bridge, renderer side:** installed only in internal browsers on `torfox://` frames.
- **Bridge, browser side:** every message is checked for:
  - the sender's browser identity;
  - main frame;
  - the committed origin;
  - the page's command allowlist;
  - typed argument schemas.
- **Native platform UI:**
  - Windows: title bar with system caption buttons, TaskDialog prompts.
  - macOS: NSMenu, traffic lights, NSAlert sheets.

## Tor
- **Client:** the official Tor Expert Bundle (C tor). Bundles are GPG-verified against the Tor Browser
  Developers key `EF6E 286D DA85 EA2A 4BA7 DE68 4E2C 6E87 9329 8290` in CI, then pinned by SHA-256.
- **`TorService`:**
  - writes `torrc` at runtime: random loopback SOCKS and control ports, cookie auth, `__OwningControllerProcess`, `ClientOnly`;
  - takes ownership over the control port;
  - tracks bootstrap and exposes `Disconnected / Connecting(n%) / Connected / Error` to the UI;
  - supports bridges (obfs4, Snowflake, WebTunnel).
- **Fail closed:** Tor-context requests are cancelled unless tor is Connected, and the proxy config has no
  DIRECT fallback.
- **Circuit isolation:** Chromium's SOCKS5 client has no authentication, so per-tab isolation via SOCKS
  credentials is impossible. TorFox uses `IsolateDestAddr`, which is weaker than Tor Browser's first-party isolation.

## Updates
- GitHub Releases on the repository named in `config/release.json`.
- The manifest is signed with minisign (Ed25519) in CI. Its trusted comment carries version and timestamp for
  anti-rollback, and it lists each platform asset's SHA-256 and size.
- The client verifies, in order: signature → strictly newer version and timestamp → asset hash and size →
  OS code signature (once configured).
- Only GitHub hosts are allowed. The previous installation is preserved on failure.

## Security assumptions
- **Chromium/CEF security:** Chromium's sandbox and site isolation are TorFox's primary defence against
  malicious content. TorFox is only as current as the CEF build it ships.
- **Operating system:** the OS, the user account and the install directory (Program Files, per-machine) are
  not compromised. TorFox does not defend against local malware running with the user's privileges.
- **Tor:** Tor provides network-level anonymity for traffic routed through it. TorFox does not provide
  Tor Browser's fingerprinting resistance; sites can still fingerprint the browser.
- **Updates:** update integrity rests on the secrecy of the minisign key held in GitHub Actions secrets and
  on GitHub's TLS. Until code-signing certificates exist, the OS gives no publisher identity.
