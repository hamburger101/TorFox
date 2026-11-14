# Stage 1 findings (2026-11-14)

What Stage 1 (the core shell) established, what it found, and what it leaves open. Every claim here comes from
a test in this repository; [`TESTING.md`](TESTING.md) says how to run them. All results were measured on
Windows 11 x64 with CEF 156.0.30 (Chromium 156.0.8078.96), using Release builds. **macOS has not been built
or run yet** (see the end of this file).

## Results at a glance
| Suite | Result |
|---|---|
| Unit tests (`torfox_unit_tests`) | 57 passed |
| End-to-end tests (test build) | 86 passed, plus 1 expected failure: the known WPAD gap below |
| Release-build platform tests | 2 passed: sandbox levels, command-line hardening |
| Mutation check (`tests/e2e/run_mutations.py`) | All 20 mutations detected |

## 1. `torfox://` is layered, and each layer is tested on its own
A web page must not reach a TorFox page. The defence has five layers:

| # | Layer | Where | Stops |
|---|---|---|---|
| 1 | Chromium's `DISPLAY_ISOLATED` scheme flag | Chromium | Renderer-initiated navigations and loads (links, `location`, iframes, fetch, …) |
| 2 | Navigation guard | `TabClient::OnBeforeBrowse` → `guards::NavigationBlockReason` | Every navigation, including browser-initiated ones |
| 3 | Resource guard | `TabClient::OnBeforeResourceLoad` → `guards::ResourceBlockReason` | Every subresource and navigation request |
| 4 | Scheme handler exists only in the Internal context | `ContextManager` | Normal, Private and Tor contexts have no `torfox` handler at all |
| 5 | Role check in the handler | `guards::DecideInternalRequest` | A request from any browser not registered as internal |

**Browser-initiated navigations bypass layer 1.** These include DevTools `Page.navigate`, and later the
address bar and command-line URLs. `BrowserInitiatedNavigationTest` covers these cases, including `blob:torfox://…`,
upper-case schemes, `..` paths, `chrome://` and `file://`.

To prove that the inner layers work without the outer ones, the test build has a fault-injection switch,
`--torfox-test-skip-guards=nav[,resource]`. It is compiled out of release builds.

| Test | Layers disabled | Evidence |
|---|---|---|
| `test_resource_guard_alone_refuses_internal_urls` | 2 | The test log shows `resource_blocked` |
| `test_web_contexts_have_no_internal_scheme_handler` | 2 and 3 | The load fails with `net::ERR_UNKNOWN_URL_SCHEME`, and the handler logs no request |

The same applies in the other direction. Internal browsers refuse web URLs, http(s) subresources and
other internal pages; see `InternalPagesStayInternalTest` and `test_internal_tab_refuses_web_urls`.

## 2. Mutation check
`tests/e2e/run_mutations.py` takes out one protection at a time, rebuilds, and runs the tests meant to catch
that change. A mutation counts as detected only if **every** listed check fails. All 20 are detected:

- **Isolation:**
  - nav guard and resource guard allow privileged schemes;
  - scheme handler registered in web contexts;
  - handler serves any browser;
  - internal browsers reach the web or the network;
  - shell page role check removed.
- **Bridge:** installed in web pages; page allowlist removed.
- **Fail-closed defaults:** downloads allowed; onion allowed in web tabs; onion resolver rule removed.
- **Network:** service contexts follow the system proxy; component updater not redirected; each of the
  four disabled background features re-enabled on its own.
- **Command line:** the policy keeps user switches (caught by unit tests). The policy **and**
  `command_line_args_disabled` are both removed in the release build (caught by the release platform test).
  So each command-line layer is needed for the build to stay locked down.

Two things the check found while it was being written:
- The first version of the navigation-guard check couldn't catch a guard that let privileged schemes
  through, because another rule happened to cover the same URLs. A `blob:torfox://` case now separates them.
- Four disable-features entries added on suspicion (`OptimizationGuideManifestBroker`, `OptimizationHints`,
  `OnDeviceModelBackgroundDownload`, `AimServerEligibilityEnabled`) made no difference when re-enabled.
  They were removed. Every remaining entry is there because a test fails without it.

## 3. Background traffic that the usual switches don't stop
`--disable-background-networking` and `--disable-component-update` are set. Even so, the Chrome runtime
contacted Google within 20 seconds of an idle start. Each request was identified from the NetLog (by host and
traffic-annotation hash) and stopped as follows:

| Request | Source | Fix |
|---|---|---|
| `clients2.google.com/time/1/current` | Network time service | Disable `NetworkTimeServiceQuerying` |
| `www.gstatic.com` translate ranker model | Translate | Disable `TranslateRankerQuery` and `TranslateRankerEnforcement` |
| `www.google.com/async/folae` | AI-mode eligibility check | Disable `AimServerRequestOnStartupEnabled` |
| `update.googleapis.com`, `edgedl.me.gvt1.com` | Component updater (optimization-guide model manifest), registered despite `--disable-component-update` | `--component-updater=url-source=http://127.0.0.1:9/,disable-pings` |

Chromium refuses to connect to port 9 (an "unsafe" port), so no component can check for updates,
download anything, or ping.

**Trade-off: no component updates means no CRLSet updates.** Chromium's certificate-revocation list
(CRLSet) is delivered as a component. TorFox therefore doesn't get revocation updates between CEF releases.
This is deliberate, because it keeps the browser from contacting Google on its own. A later stage needs to
decide how revocation data reaches users. One option is to ship it through TorFox's own signed update channel.

## 4. Proxy auto-discovery (WPAD): a known gap
On Windows, "Automatically detect settings" is on by default (Settings → Network → Proxy). With it on,
Chromium sends a WPAD DHCP query and looks up the host `wpad`, which can leak to the local network.

- **Fixed:** service contexts never auto-discover. The global context, Local State and the Internal
  context all have their proxy pref set to `direct`. This cut the contexts that read the system proxy from
  4 to 1. `test_service_contexts_never_autodiscover_proxies` checks it, and a mutation backs it up.
- **Not fixed:** the Normal context still auto-discovers once at start-up. Its network context is created with
  system proxy settings before any per-context pref can apply. `test_idle_browser_does_not_autodiscover_proxies`
  is marked as an expected failure on machines where auto-detect is on, so a fix will show up as an
  unexpected success.
- **Not an option:** `--no-proxy-server`. Command-line proxy settings override per-context prefs, so it
  would also override the Tor context's SOCKS proxy.
- **Decision needed in Stage 4:** the Normal context's default proxy mode. One option is "system proxy,
  without auto-discovery".

## 5. WebSockets aren't seen by CEF's request handlers
CEF never passes WebSocket handshakes to `OnBeforeResourceLoad`, so the resource guard can't stop them.

- **Onion WebSockets from web tabs:** these are still stopped, by the resolver rule
  `MAP *.onion ~NOTFOUND`. `test_onion_names_never_reach_the_resolver` shows a `wss://….onion` attempt in
  the NetLog, with no DNS resolution and no connection. `test_resolver_rule_alone_keeps_onion_names_local`
  shows that the rule holds even with both guards disabled.
- **Stage 4 tracker blocking can't rely on `OnBeforeResourceLoad` for WebSockets** and needs another
  mechanism. This is an open design question.

## 6. Release builds can't be weakened from the command line
There are two independent layers:
1. CEF's `command_line_args_disabled`, which is set in release builds.
2. TorFox's switch policy (`src/core/cmdline/policy.cc`). It drops every user switch, keeps only absolute
   http(s) URLs, and then adds TorFox's forced switches.

`test_command_line_cannot_weaken_torfox` starts the **release** build with 16 hostile switches, including
`--no-sandbox`, `--single-process`, `--remote-debugging-port`, `--proxy-server`, `--host-resolver-rules`,
`--disable-web-security`, `--disable-features=NetworkServiceSandbox`, `--user-data-dir`, `--log-net-log` and a
redirected component updater. It then checks the following:
- the page loads directly;
- every process is still sandboxed;
- no DevTools port is open;
- no file appears outside the profile.

## 7. Sandbox levels (release build)
Measured by `tests/platform/win_process_inspect.py` from each process's token:

| Process | Sandbox level |
|---|---|
| Renderers | Untrusted integrity, restricted token |
| Network service | Exactly one, in an AppContainer (LPAC) |
| GPU | Low or Untrusted |
| Every child | Started without `--no-sandbox` or `--single-process` |

## 8. Chrome's "Default" profile
The Chrome runtime always creates an on-disk `User Data/Default` profile. TorFox never browses in it.
`test_no_history_in_default_profile` visits a page and then checks that `History`, `Favicons` and `Top Sites`
have no rows and that the URL appears nowhere in the profile.

## 9. Limits of this evidence
- **Idle window:** the idle-traffic test covers 20 seconds locally and 60 seconds on CI. Services with
  longer timers (hourly or daily) aren't covered yet.
- **Platforms:** the E2E and platform tests run on Windows only. They use Win32 APIs, DevTools over loopback
  and NetLog.
- **macOS:**
  - The bundle layout, helper apps, `Info.plist` files, entry points and platform code (`src/browser/mac*`,
    `src/browser/main/mac_*`) are written but **have never been compiled**. The CI job runs only on pull requests
    and manual dispatch, because macOS minutes are expensive on a private repository.
  - The native menu has App, Edit and Window menus. TorFox's own commands are keyboard shortcuts only, for now.
- **Extension registry:** the Stage 0 finding about Chrome's external-extension registry keys is handled
  by `--disable-extensions`. A test that plants such a key hasn't been written yet. Writing under
  `HKCU\Software\Google\Chrome\Extensions` on a developer machine would also affect a real Chrome
  installation, so that test belongs in CI only.
