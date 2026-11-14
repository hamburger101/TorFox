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
- **Command-line hardening:** release builds have two independent layers.
  1. CEF's `command_line_args_disabled`.
  2. TorFox's policy (`src/core/cmdline/policy.cc`). It drops every user switch, keeps only absolute
     http(s) URL arguments, and then forces TorFox's own switches:
     - `--site-per-process`
     - `--host-resolver-rules="MAP *.onion ~NOTFOUND"`
     - `--disable-extensions`
     - Chromium background services off: background networking, component update, domain reliability,
       client-side phishing detection, sync, default apps, pings
     - `--component-updater=url-source=http://127.0.0.1:9/,disable-pings`. Port 9 is refused by Chromium,
       so no component can update or ping. As a result, CRLSets aren't updated either.
     - `NetworkServiceSandbox` enabled
     - Four background features disabled, each needed according to the quiet-startup test:
       `NetworkTimeServiceQuerying`, `TranslateRankerQuery`, `TranslateRankerEnforcement` and
       `AimServerRequestOnStartupEnabled`. `HttpsUpgrades` is disabled too, in favour of TorFox's own HTTPS-first.

     Test builds (`TORFOX_TEST_HOOKS=ON`) also keep a short list of test switches; release builds keep none.
     See [`TESTING.md`](TESTING.md).

## Request contexts (isolation boundaries)
| Context | Storage | Network | Notes |
|---|---|---|---|
| Base (global) | Disk, never hosts content | Proxy pref `direct` (also in Local State) | Holds the strict baseline prefs |
| Internal | Off-the-record (OTR) | None; http(s) requests are cancelled; proxy pref `direct` | Only context with the `torfox` scheme handler |
| Normal | Disk profile | Direct, or the system's proxy settings if the user turns that on | HTTPS-first, trackers, 3P cookies, pop-ups, permissions. `.onion` is cancelled and offered in a Tor tab |
| Private | Its own OTR profile, one per private window | As Normal | No history; its downloads list is its own and is dropped with the window |
| Tor | Its own OTR profile | SOCKS5 to tor only | See below |

The Tor context:
- **Proxy:** the proxy pref is applied in `OnRequestContextInitialized` and read back before any browser is created.
  It is `fixed_servers`, never DIRECT, with bypass list `<-loopback>`.
- **Traffic:** every request goes through Tor: onion and clearnet subresources alike.
- **Hardening:** `webrtc.ip_handling_policy = disable_non_proxied_udp` and prediction off.
  Camera, mic, location, notifications and external protocols are hard-denied, and history is never recorded.

A tab changes context only by replacing its browser. Popups inherit the opener's context.

No context runs proxy auto-discovery (WPAD) unless the user turns on *Use the system's proxy settings*.
The service contexts (Base, Local State and Internal) always use `direct`. The web contexts use `direct`
or `system`, from that setting. The Normal profile's network context starts before TorFox can set prefs,
so TorFox writes the proxy mode into its Preferences file before CEF starts
([`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md) §3.6). TorFox never uses `--no-proxy-server`, because it would
override the Tor context's proxy.

Chrome's runtime also keeps an on-disk `User Data/Default` profile. TorFox never browses in it, and a test
checks that it stays empty of browsing data.

## Browsing data
- **History and bookmarks** live in TorFox's own SQLite database, `<user data>/places.db` (pinned SQLite
  amalgamation, `secure_delete` on, WAL truncated after removals). Chromium's history service is switched off
  in every context (`history.saving_disabled`), so this is the only record. The schema and its rules are in
  `src/core/browsing/places.cc`, unit-tested without CEF.
- **What is recorded:** successful page loads in Normal windows only. Private windows, TorFox pages and error
  pages are never recorded. Only http(s) URLs with a host are stored (`IsStorableUrl`).
- **Private windows** each create a new off-the-record context when they open, verified before the window
  appears. The context belongs to the window and is dropped when it closes. Every tab and link in it stays in
  that context.
- **Pages:** `torfox://bookmarks/` and `torfox://history/` reach the data only through their own bridge
  commands (`bookmarks.*`, `history.*`). The shell keeps only the commands it needs (star, folder menus).

## Privacy (web contexts)
All of it is decided in the browser process (`TabClient`, `PrivacyService`), from Settings > Privacy &
Security. The pure rules live in `src/core/privacy/` and are unit tested without CEF.
- **HTTPS-first:** `OnBeforeBrowse` retries a main-frame `http://` GET over https (`https_first::UpgradedUrl`:
  public domain names only). A failure that http might fix, or an https→http redirect, asks before loading
  http. Accepted sites are remembered in memory: one list for normal windows, one per private window.
- **Trackers:** `OnBeforeResourceLoad` cancels a subresource whose host is on the compiled-in EasyPrivacy
  domain list when the request is third-party (registrable domains differ, using the Public Suffix List).
- **Cookies and pop-ups:** `profile.cookie_controls_mode`, re-applied to every open context when the
  setting changes. Pop-ups are decided in `OnBeforePopup`, since Chromium's own blocker is set to allow;
  allowed ones open as tabs.
- **Permissions:** camera and microphone (`OnRequestMediaAccessPermission`) and clipboard
  (`OnShowPermissionPrompt`) are denied, or asked about every time. Everything else is denied. Chromium's
  stored answer is reset right after each prompt, so nothing is remembered per site.
- **Email and phone links:** handled in `OnBeforeBrowse`. After a click, the user is asked, and only a
  checked `mailto:`/`tel:` URL reaches the OS (`external_protocols::LaunchableScheme`).
  `OnProtocolExecution` refuses everything.
- **Clear on exit:** `clear_data::Clear` deletes the chosen categories' files after `CefShutdown`, and again
  before `CefInitialize` (crash recovery). Links are removed, never followed. TorFox's own history is
  cleared in `places.db`. Chromium's per-site scores are always erased from Preferences, and zoom levels
  with history (`chromium_profile::ScrubSiteRecords`).

## Downloads
- **Policy:** `downloads::Decide(disable, ask)` (`src/core/downloads/policy.cc`) gives Block, Ask or Allow.
  *Disable* overrides *Ask*. `TabClient` applies it in `CanDownload` (which in CEF 156 sees every download),
  again in `OnBeforeDownload` (before any file is named), and in `OnDownloadUpdated`. Only web tabs download.
- **Prompt:** `dialogs::Confirm` → `platform::ShowConfirmPrompt`: TaskDialog (Windows) or NSAlert sheet
  (macOS), Cancel as the default. It belongs to the tab: closing the tab or turning downloads off cancels
  it, and a dialog that cannot be shown counts as no.
- **Paths:** TorFox names every file (`SafeFileName`, `UniquePath`): never an existing file or one another
  waiting download will use, and short enough for Windows' `MAX_PATH`. Chromium writes to `name.crdownload`,
  and `DownloadService` renames the file when it is complete. A failed download is cancelled; a partial file
  Chromium leaves behind is deleted 2 s later. Bytes received before the decision go to
  `<profile>/Pending Downloads` (Chromium's `download.default_directory`), which is emptied at start-up.
- **State:** `DownloadService` (`src/browser/app/download_service.cc`) keeps the list in memory only.
  Entries have TorFox's own ids; Chromium's updates are matched by (profile, Chromium id), because each
  profile numbers its downloads from 1. Chromium's own record of downloads in the normal profile is erased
  before CEF starts and after it shuts down (`chromium_profile::ScrubDownloadRecords`).
  `torfox://downloads/` reaches it through `downloads.*` bridge commands, filtered by window: private
  windows see only their own entries. There is no command to open a file.
- **Pages cannot write files another way:** the File System Access pickers are turned off
  (`--disable-blink-features=FileSystemAccessLocal`), and the page context menu has no save items.

## Commands, shortcuts and menus
- One command list (`src/core/browsing/shortcuts.h`) serves keyboard shortcuts, the app menu and the shell
  buttons. The key tables for Windows and macOS are separate and unit-tested; macOS keeps Cmd+Left/Right for
  text editing.
- Shortcuts are caught in the browser process (`OnPreKeyEvent`) before any page sees them, in web pages and
  TorFox pages alike.
- Menus are native CEF Views menus (`CefWindow::ShowMenu`). TorFox builds the page context menu itself, so
  Chromium's Save, Print, View source and Inspect items never appear.

## Internal UI (`torfox://`)
The UI is HTML/TypeScript (Lit, built by Vite into a compiled-in resource table), rendered in a separate
privileged browser. A web page reaching a TorFox page has to get past five layers:
1. Chromium's `DISPLAY_ISOLATED` check.
2. TorFox's navigation guard (`OnBeforeBrowse`), which also covers browser-initiated navigations.
3. The resource guard (`OnBeforeResourceLoad`).
4. A scheme handler that exists only in the Internal context.
5. The handler's own check of the requesting browser's role.

The decisions for layers 2, 3 and 5 are pure functions in `src/core/url/guards.cc`, unit-tested on their own.
Each layer is also tested end-to-end with the outer layers switched off (test builds only), see
[`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md) §1. The rest of the setup:
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
  - macOS: NSMenu, traffic lights, NSAlert sheets. The main menu has App, Edit and Window menus; the
    Stage 2 commands are reachable through shortcuts and the in-window app menu. macOS is built only by CI.

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
