# Stage 0 feasibility findings (2026-11-14)

Evidence from the spikes in `spikes/cefspike/`. To reproduce, build with
`-DTORFOX_BUILD_SPIKES=ON` and run `python spikes/cefspike/run_spikes.py <a|b|c|d> --exe <cefspike.exe>`.
All runs were on Windows 11 x64 with CEF 156.0.30 (Chromium 156.0.8078.96), built with
Visual Studio 2026 (MSVC 19.51). macOS has not been exercised yet.

| # | Question | Result |
|---|---|---|
| A | Does VS 2026 build CEF's bootstrap-DLL model, with the sandbox on? | **Yes** |
| B | Can one tab's BrowserView close without closing its window? | **Yes**, with a specific sequence |
| C | Per-context SOCKS proxy, fail-closed behaviour, isolation? | **Yes** (Release builds) |
| D | Do the privacy prefs work for Alloy-style browsers? | **Mostly**; HTTPS-first needs our own implementation |

## A — Bootstrap DLL + sandbox
- `bootstrap.exe` renamed to `cefspike.exe` loads `cefspike.dll` (exports `RunWinMain`), and `sandbox_info` is non-null.
- Integrity levels of the child processes, measured by `tests/platform/win_process_inspect.py`:
  - renderer: **Untrusted**, restricted token
  - storage service: **Untrusted**, restricted
  - GPU: **Low**, restricted
  - network service: **Medium, not sandboxed** by default. With `--enable-features=NetworkServiceSandbox`
    it runs at **Low in an AppContainer (LPAC)**, and pages still load.
    → TorFox enables the network-service sandbox. The installer must apply CEF's LPAC ACL
    (`icacls <dir> /grant *S-1-15-2-2:(OI)(CI)(RX)`) to the install directory.
- **Finding:** in the CEF log, Chrome's external-extension registry loader reads
  `Software\Google\Chrome\Extensions`. Other software on the machine could use those keys to plant extensions.
  → TorFox passes `--disable-extensions`. A test with a planted registry key must prove that nothing loads.
- CEF objects held past `CefShutdown()` trigger a fatal shutdown check. Product code must release
  every reference before shutdown.

## B — Closing one tab
This sequence works for both the visible and a hidden tab:
1. `CloseBrowser(false)`, which still runs unload handlers.
2. `DoClose` returns **true**.
3. Remove the BrowserView from its panel **and drop every reference** to it.
4. `OnBeforeClose` follows.

The window stays valid and the remaining tabs keep working.

Removing the view while still holding a reference leaves the browser half-closed, and the process later crashes.

## C — Per-context proxy and isolation
- `SetPreference("proxy", {fixed_servers, socks5://127.0.0.1:P, bypass_list "<-loopback>"})` on the Tor
  context returns ok and reads back. The Normal, Private and global contexts stay `{"mode":"system"}`.
- **Remote DNS:** the fake SOCKS5 server saw ATYP=domain for both clearnet hostnames and `.onion`, so
  nothing is resolved locally.
- **Fail closed:** with the proxy pointed at a dead port, both hostnames and `127.0.0.1` URLs fail with
  `ERR_PROXY_CONNECTION_FAILED`, so `<-loopback>` works. The fixture HTTP server got **zero** direct connections.
- **Proxy changes at runtime** take effect immediately, which covers tor restarting on a new port.
- **`.onion` in the Normal context:** `--host-resolver-rules="MAP *.onion ~NOTFOUND"` gives `ERR_NAME_NOT_RESOLVED`.
  NetLog verification that no DNS query leaves is pending (Stage 1/5 test).
- **Isolation:** cookies set in the Tor, Private and Normal contexts are each visible only in their own
  context. Two contexts with an empty `cache_path` are distinct (`IsSame` and `IsSharingWith` are false).
- **Upstream issue (Debug only):** creating a browser in *any* off-the-record context triggers
  `FATAL: observer_list.h:371 Observers can only be added once!` in CEF **Debug** builds.
  - It happens with or without TorFox's switches, and Release builds behave correctly.
  - → OTR-dependent tests run against Release builds, and the issue gets reported to CEF.
  - The fallback, a disk-backed Tor profile wiped on exit, works in Debug.
- **Finding:** each profile opens a connection to `www.google.com:443` by itself. Through the Tor context it
  arrived via SOCKS; in a Normal context it would go out directly.
  - Disabling `SearchEnginePreconnector` / `SearchEnginePreconnect2` did **not** stop it.
  - → Stage 1 adds a NetLog "quiet startup" test that uses traffic annotations to identify and disable
    every request TorFox didn't ask for. TorFox must not ship until it passes.
- Chromium keeps one value per switch, and CEF sets its own `--disable-features`. TorFox's disabled features
  must be **merged** into that value in `OnBeforeCommandLineProcessing`, not appended as a second switch.

## D — Privacy prefs for Alloy-style browsers
Every pref below could be set (`CanSetPreference` true) and read back:

| Pref | Context | Value |
|---|---|---|
| `net.network_prediction_options` | Normal and Tor | 2 |
| `profile.cookie_controls_mode` | Normal | 1 |
| `https_only_mode_enabled` | Normal | true |
| `webrtc.ip_handling_policy` | Normal | `default_public_interface_only` |
| `webrtc.ip_handling_policy` | Tor | `disable_non_proxied_udp` |

- Setting `cookie_controls_mode` on Normal did **not** change Private or global (both still 2). Every
  context must be set explicitly.
- **WebRTC ICE gathering:**
  - Tor context: **zero** candidates.
  - Normal context: zero candidates.
  - Default policy: only mDNS-obfuscated `.local` host candidates, never raw IPs.
- **HTTPS:** Chromium's default HTTPS-Upgrades rewrote `http://example.com` to https *with or without*
  `https_only_mode_enabled`. For an HTTP-only site the upgraded load ended in `ERR_TIMED_OUT` / an internal 307,
  with no clean fallback and no interstitial.
  → TorFox implements its own HTTPS-first policy with an explicit interstitial, and disables Chromium's
  built-in upgrade so the behaviour is deterministic and testable with local fixtures.

## Tor bundle
- `*.torproject.org` connections are reset during the TLS handshake on the development network, so the
  bundles are fetched by `.github/workflows/fetch-tor.yml`.
- The Tor Browser 16.0.1 expert bundles (windows-x86_64, macos-x86_64, macos-aarch64) carry valid
  signatures whose primary key is `EF6E 286D DA85 EA2A 4BA7 DE68 4E2C 6E87 9329 8290` (signing subkey
  `022D A248 432D 2A0E 0F54 E65E 316C 1FAC D62D 07D9`). They were verified twice:
  - in CI, using the key from WKD;
  - again locally, using the key from keys.openpgp.org.

  They are pinned by SHA-256 in `deps.lock.json`.
- `tor.exe --version` reports **Tor 0.4.9.16**, with Libevent 2.1.13, OpenSSL 3.5.9 and Zlib 1.4.
- **Licensing:** this tor build says it is covered by the **GPL-3.0**. TorFox (MPL-2.0) runs tor as a separate
  program, which is aggregation. Release packages must still include the GPL text and a pointer to the
  corresponding tor source.
- The bundle contains `lyrebird`, `conjure-client`, `pt_config.json` and the geoip files.
  - `lyrebird` now provides meek_lite, obfs4, WebTunnel **and Snowflake**.
  - `pt_config.json` ships built-in bridges: 7 obfs4, 2 Snowflake, 1 meek, with obfs4 as the recommended default.
  - This is enough for Settings → Tor Network → Built-in bridges.

## Plan changes from these findings
1. Add `--enable-features=NetworkServiceSandbox` and `--disable-extensions`, merging into CEF's feature switches.
2. The installer applies the LPAC ACL to the install directory.
3. Tab close follows the sequence in B.
4. OTR contexts for Private and Tor, with E2E tests on Release builds.
5. A quiet-startup NetLog test moves up to Stage 1 and blocks release until every background request is gone.
6. HTTPS-first is TorFox's own implementation; Chromium's HTTPS-Upgrades is disabled.
