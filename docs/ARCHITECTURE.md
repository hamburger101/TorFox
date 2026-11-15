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
| Normal | Disk profile | Direct, or the system's proxy settings if the user turns that on | HTTPS-first, trackers, 3P cookies, pop-ups, permissions. `.onion` is cancelled; typed onion addresses open in a Tor window |
| Private | Its own OTR profile, one per private window | As Normal | No history; its downloads list is its own and is dropped with the window |
| Tor | Its own OTR profile, shared by all Tor windows until New identity | SOCKS5 to tor only | See below |

The Tor context:
- **Proxy:** the proxy pref is applied in `OnRequestContextInitialized` and read back before any browser is created.
  It is `fixed_servers`, never DIRECT, with bypass list `<-loopback>`. Until tor reports its SOCKS port, the
  port is a placeholder (9) that nothing listens on.
- **Traffic:** every request goes through Tor: onion and clearnet subresources alike. Chromium sends the host
  name to tor (SOCKS5 address type 3), so nothing is resolved locally.
- **Hardening:** `webrtc.ip_handling_policy = disable_non_proxied_udp`, prediction off, third-party cookies
  always blocked. Camera, microphone, clipboard, location, notifications and other apps are refused without
  asking, local addresses are refused, and history is never recorded. The system-proxy setting never applies.
- **Fail closed, three layers:** the Tor tab's guard (`TabClient::TorHoldsBack`), the context's `TorGate` for
  requests no tab makes (service workers), and the proxy itself. Each is tested on its own
  ([`STAGE5_FINDINGS.md`](STAGE5_FINDINGS.md) §1).

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
- **Saved tabs** (*Settings > General > On startup > Restore previous tabs*): the tabs of Normal windows,
  in `<user data>/session.json`, written atomically a second after they change (so a crash keeps them) and
  only while the choice is on and *Clear when TorFox closes > History* is off. Otherwise the file is removed:
  when either setting changes, at start and at exit. Private and Tor windows are never saved; only http(s)
  URLs without an onion host and TorFox's own pages are kept, and a damaged or foreign file restores nothing
  (`src/core/browsing/session.cc`). A save that would leave no window is skipped, so the file keeps the
  windows that were open when the last one closed.
- **Search** (*Settings > Search engine*): DuckDuckGo, DuckDuckGo HTML (`html.duckduckgo.com/html/`) or
  DuckDuckGo Lite (`lite.duckduckgo.com/lite/`); in Tor windows the same page on DuckDuckGo's onion service
  (`src/core/url/search.cc`). Used by the address bar, the new tab page and the page menu's selection search.

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
- **Client:** the official Tor Expert Bundle (C tor; 16.0.1, tor 0.4.9.16). Bundles are GPG-verified
  against the Tor Browser Developers key `EF6E 286D DA85 EA2A 4BA7 DE68 4E2C 6E87 9329 8290` in CI, then
  pinned by SHA-256. tor and its pluggable transports are copied to `<TorFox>/tor` (Windows) or
  `Contents/Helpers/tor` (macOS); `tor-gencert` is left out.
- **Tor windows:** private windows whose context is the Tor context. A normal window never shows a Tor
  page, and a Tor window never shows a non-Tor page; links between them open the other kind of window.
- **`TorService`** (`src/browser/app/tor_service.cc`, protocol code in `src/core/tor/`):
  - writes `torrc` before each start (`src/core/tor/torrc.cc`): loopback SOCKS and control ports chosen by
    tor, cookie auth, `__OwningControllerProcess`, `ClientOnly`, `IsolateDestAddr`, bridges;
  - authenticates with SAFECOOKIE (checking tor's HMAC first), takes ownership, and follows bootstrap
    through `STATUS_CLIENT` events;
  - on Windows, starts tor suspended inside a kill-on-close job object;
  - exposes `off / starting / connecting(n%) / connected / error` to the UI, and restarts tor after 2, 5
    and 15 s when it stops unexpectedly;
  - restarts tor when a bridge setting changes; *New identity* drops the context and sends `SIGNAL NEWNYM`.
- **Fail closed:** see the Tor context above. A Tor tab's request starts only while tor is connected, and
  the proxy config has no DIRECT fallback.
- **Circuit isolation:** Chromium's SOCKS5 client has no authentication, so per-tab isolation via SOCKS
  credentials is impossible. TorFox uses `IsolateDestAddr`, which is weaker than Tor Browser's first-party isolation.

## Updates
User-facing behaviour and limits: [`UPDATING.md`](UPDATING.md). Making a release: [`RELEASING.md`](RELEASING.md).
- **Configuration.** `config/release.json` names the repository and the minisign public key. CMake checks
  both and compiles them in (`release_config.h`). With either missing the updater is *not configured* and
  never connects. Test builds can replace both with `--torfox-test-update-config` (a fake GitHub and a key
  made per test run); release builds never read that switch.
- **Pure rules** (`src/core/update/`, no CEF, unit-tested):
  - `minisign`: prehashed minisign (`ED`, Ed25519 over BLAKE2b-512) with libsodium. Key ID, file
    signature and the global signature over the trusted comment.
  - `release`: the manifest (`format` 1, `product` "TorFox", `version`, `timestamp`, `notes`, `assets` by
    platform with `name`, `size`, `sha256`). The trusted comment must be exactly
    `torfox-release version:<v> timestamp:<t>`. Strictly newer versions only, and never a release older
    than the newest seen (`update.json`). Host allowlist: `https://` `api.github.com`, `github.com`,
    `objects.githubusercontent.com`, `release-assets.githubusercontent.com`, compared as exact origins.
    Reading GitHub's *latest release* answer: no drafts or pre-releases, and file URLs must be exactly
    `https://github.com/<repo>/releases/download/<tag>/<name>`.
  - `sha256`: streaming SHA-256 and file hashing (libsodium).
- **`UpdateService`** (browser process, UI thread). States `not_configured / off / idle / checking /
  up_to_date / available / downloading / ready / error`, shown on `torfox://settings` through the bridge
  (`update.status`, `update.subscribe`, `update.check`, `update.download`, `update.install`, settings page
  only).
  - Requests use `CefURLRequest` in an off-the-record context of their own, with
    `UR_FLAG_STOP_ON_REDIRECT`: TorFox checks each `Location` against the allowlist before following it (at
    most 5). No cookies, no cache, no credentials (`GetAuthCredentials` refuses).
  - The manifest and signature are limited to 64 KiB, the file to its signed size (at most 1 GiB). The file
    is hashed while it downloads into `Updates/<name>.part` and renamed only when size and hash match.
  - Automatic checks: a tick two minutes after start and then every hour. It checks only when the last
    check (or attempt) was more than a day ago.
  - *Install*: the file is hashed again, all windows close, and after `CefShutdown`
    `platform::LaunchUpdate` hashes it once more. On Windows it holds the file open without write sharing
    while it hashes it and starts the installer
    (`/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE`, UAC). On macOS it opens the DMG.
- There is no OS code-signature check: TorFox has no code-signing certificates (it is not distributed
  through app stores).

## Security assumptions
- **Chromium/CEF security:** Chromium's sandbox and site isolation are TorFox's primary defence against
  malicious content. TorFox is only as current as the CEF build it ships.
- **Operating system:** the OS, the user account and the install directory (Program Files, per-machine) are
  not compromised. TorFox does not defend against local malware running with the user's privileges.
- **Tor:** Tor provides network-level anonymity for traffic routed through it. TorFox does not provide
  Tor Browser's fingerprinting resistance; sites can still fingerprint the browser.
- **Updates:** update integrity rests on the secrecy of the minisign key, held only as a GitHub Actions
  secret, not on GitHub's TLS. TLS makes it harder for a network attacker to hide new releases from TorFox,
  which a signature alone cannot prevent (an old answer can never bring back an older release). TorFox has no code-signing certificates, so
  the OS gives no publisher identity.
