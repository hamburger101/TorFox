# Stage 5 findings (2026-11-15)

What Stage 5 (Tor) built, what it found, and what it leaves open. Every claim here comes from a test in
this repository; [`TESTING.md`](TESTING.md) says how to run them. All results were measured on Windows 11
x64 with CEF 156.0.30 (Chromium 156.0.8078.96) and the Tor Expert Bundle 16.0.1 (tor 0.4.9.16), using
Release builds. macOS is built by CI only. None of the Stage 5 behaviour has been run on macOS, and its
macOS code has never been compiled.

> **TorFox is not Tor Browser.** Tor windows send all their traffic through Tor and fail closed, but they
> do not have Tor Browser's protections against fingerprinting, and they keep sites apart only by giving
> each destination address its own Tor circuits (§6).

## Results at a glance
| Suite | Result |
|---|---|
| Unit tests (`torfox_unit_tests`) | RESULTS_UNIT |
| End-to-end tests (test build, no network) | RESULTS_E2E |
| Tor tests with the bundled tor (network, `TORFOX_LIVE_TESTS=1`) | RESULTS_LIVE |
| Mutation check (Stage 5 mutations) | RESULTS_MUT |

## 1. What was built

### Tor windows
- **Opening one:** *New Tor window* in the app menu, Alt+Shift+N on Windows (⌘⌥N on macOS), *Open link in
  Tor window* on a link, or typing an onion address in a normal window.
- **What a Tor window is:** a private window whose every connection goes through the bundled tor. All Tor
  windows share one off-the-record context until *New identity*. History is not recorded, and the context is
  off the record: TorFox confirms it has no storage path before any Tor window opens. No Tor-specific test
  looks for files left on disk (the private-window test does, for the same kind of context).
- **How it looks:** a purple tab strip and a Tor badge showing *Connecting n%* (with a progress bar),
  *Connected*, or *Not connected* with the reason in its tooltip. After an error that TorFox will not
  retry by itself, a *Try again* button appears. The broom button is *New identity*. Onion sites get an
  *Onion* badge in the address bar.
- **Onion addresses:**
  - Only version 3 addresses (56 base32 characters) are accepted, and their checksum is checked
    (SHA3-256 over `".onion checksum" ‖ public key ‖ version`, TorFox's own Keccak implementation, tested
    with the FIPS 202 vectors). A mistyped address gets a message instead of an attempt.
  - Anything that names an onion address and is not exactly one (`what is x.onion`, `ftp://…`, a v2
    address) is refused. None of it is ever sent to a search engine.
  - In a normal window, a valid onion address opens in a Tor window. Normal windows still never connect to
    onion hosts or resolve them (Stage 1).
- **Search** in Tor windows uses DuckDuckGo's onion service
  (`duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion`).
- **Refused in Tor windows:**
  - addresses on this computer and the local network (typed, linked, embedded, or fetched by a worker);
  - links that open other apps (`mailto:`, `tel:`);
  - camera, microphone and clipboard, without asking, whatever Settings says for normal windows;
  - *Open link in private window* (Tor windows offer *Open link in new Tor window* instead).
- **Always on in Tor windows:**
  - third-party cookies are blocked, even when Settings allows them in normal windows;
  - WebRTC may use no route but the proxy, and SOCKS cannot carry UDP, so no ICE candidate is gathered at all.
- **Downloads** in Tor windows go through Tor like everything else. The download prompt adds a warning
  that a downloaded file opened later can connect to the internet without Tor.

### The tor process (`TorService`)
- **When it starts:** with the first Tor window, or when TorFox starts if *Settings > Tor > Start Tor* is
  *When TorFox starts*. It runs the bundled tor from `<TorFox>/tor` (Windows) or
  `Contents/Helpers/tor` (macOS).
- **Its configuration:** `torrc` is written in `<profile>/Tor` before each start, with an empty defaults
  file so tor reads nothing else:
  - `SocksPort 127.0.0.1:auto ExtendedErrors IsolateDestAddr` and `ControlPort 127.0.0.1:auto`:
    loopback only, ports chosen by tor and read back from `ControlPortWriteToFile`;
  - `CookieAuthentication 1`, `__OwningControllerProcess <TorFox's pid>`, `ClientOnly 1`,
    `AvoidDiskWrites 1`, `SafeLogging 1`, `DormantCanceledByStartup 1`;
  - the GeoIP files, and the bridge lines with their transport (below).
- **The control session:**
  - SAFECOOKIE authentication: TorFox checks tor's HMAC before sending its own, so a process listening on
    the port that does not know the cookie gets nothing (`TorController.NeverAnswersAPortThatCannotProveItKnowsTheCookie`);
  - `TAKEOWNERSHIP` (tor exits when the session closes);
  - `SETEVENTS STATUS_CLIENT` for bootstrap progress, and `GETINFO net/listeners/socks` for the SOCKS port.
- **Applying the port:** the Tor context's proxy starts on a placeholder port (9) that nothing listens on.
  It is switched to tor's SOCKS port as soon as tor reports it, and read back.
- **Process lifetime (Windows):** tor is started suspended, put in a kill-on-close job object, then
  resumed. If TorFox dies, Windows ends tor.
- **When tor stops unexpectedly:** the state becomes *Not connected* with the reason, and TorFox starts it
  again after 2, 5 and then 15 seconds. Pages asked for meanwhile wait and load once tor is connected.
- **Bridges** (*Settings > Tor*):
  - *None*;
  - *Built-in*: obfs4, Snowflake or meek, using the lines in the bundle's `pt_config.json`;
  - *Custom*: lines pasted from bridges.torproject.org (obfs4, WebTunnel, Snowflake, meek, or plain relays).

  Custom lines are checked when saved (Settings refuses the whole value if one line is not a bridge line
  TorFox can use) and again when `torrc` is written. Changing a bridge setting restarts tor with it.
  Choosing *Custom* with no lines leaves tor stopped with a message, instead of connecting without bridges.
- **New identity:** opens a new Tor window first (so TorFox never runs out of windows), closes every old
  Tor window, drops the shared Tor context, and sends `SIGNAL NEWNYM`.

### Fail closed: three layers, each tested on its own
1. **The Tor tab's own guard** (`TabClient::TorHoldsBack`): while tor is not connected, no navigation or
   subresource of a Tor tab starts. The address asked for is kept, and loaded when tor connects.
2. **The Tor context's gate** (`TorGate` in `context_manager.cc`): requests that no tab makes, such as a
   service worker's own fetches, follow the same rules, local addresses included.
3. **The proxy:** `socks5://127.0.0.1:<port>` with no `DIRECT` fallback and with loopback not bypassed. A
   tor that is not there means *connection refused*, never a direct connection.

The system-proxy setting (Stage 4) never changes the Tor context.

## 2. Changes from the plan
- **Tor windows instead of Tor tabs.** The plan put Tor tabs in normal windows. Separate windows keep Tor
  and non-Tor pages from ever sharing a window, make it obvious which kind of page is in front of you, and
  stop a click or drag from carrying a page across.
- **TorFox checks onion checksums itself.** The plan left that to tor. Checking first means a mistyped
  address is refused with a clear message and never offered to a search engine.
- **No bridges were needed on this machine.** The plan expected the network here to need them, because
  torproject.org's websites are blocked here. The Tor network is not: the bundled tor bootstrapped to
  100% directly (§4).
- **Tor's notices log is kept only in test builds** (`<profile>/Tor/tor-notices.log`). Release builds keep
  no log on disk.

## 3. What testing found

### 3.1 Real tor refused TorFox's configuration
The first run of `RealTorTest` showed the bundled tor exiting at once with code 1, before it wrote any log.
Running it by hand gave the reason: `Log notice file "C:\\…\\tor-notices.log"` was read with the quotes
and doubled backslashes as part of the file name. Tor unquotes a value only when the whole value is
quoted. The scripted tor had accepted the line, so only the real one could find this.

The line is now `Log "notice file C:\\…"`. A unit test checks its exact form, and `RealTorTest` also runs
`tor --verify-config` on the release form of the configuration (`Log notice stdout`).

### 3.2 Pages in a window opened on an address were "hidden"
The Stage 1 permissions test began to hang: `getCurrentPosition` never called back, and TorFox's
permission handler was never asked. The Stage 4 build, which had passed the same test, hung the same way
on the same day, so the cause was timing, not Stage 5.

The cause: a window opened on a web address (the test start URL, a URL given to a second launch, a Tor
window opened for an onion address) created its first web browser before the window was shown. Chromium
then treated that page as hidden. `document.visibilityState` was `"hidden"` on screen, so timers were
throttled and permission requests waited for the tab to become visible, which never happened.

Every window now opens on the new-tab page and loads its address once it is shown, the same path as typing
it. `test_a_window_opened_on_an_address_shows_it_as_visible` and the onion-window test check
`visibilityState`.

### 3.3 The network quality estimator asks the resolver about every host
The DNS test found resolver requests for Tor-window hosts, including `~notfound`, which is the Stage 1
resolver rule rewriting an onion name. They come from Chromium's network quality estimator, which asks
whether each host is on the local network. Its requests are restricted to the resolver cache and the hosts
file (task sequence `[4, 9]`), so they never leave the computer.

The test now uses no resolver rule for the Tor window's host, so a leak would show the real name. It
checks:
- that no resolver job or DNS transaction starts for any Tor-window host or onion name;
- that every resolver request for one is restricted to the cache and hosts file;
- with a control, that a normal window's lookup of an unmapped name does show up as a real lookup.

### 3.4 Checking the job object needs a behaviour test
`IsProcessInJob` on tor is true here whether or not TorFox makes a job: the shell the tests run in is
itself in one, and children inherit it. The behaviour test terminates only TorFox's main process, using a
tor that ignores its owner, and requires tor to die.

### 3.5 Profile folders whose names tor cannot read
Tor reads paths in the system code page, so a profile under `C:\Users\José` cannot be written into
`torrc` as is. TorFox gives tor the folder's 8.3 short name instead (`platform::AsciiPath`). A test with a
`profilé-тест` folder checks this with the scripted tor and with the real one. The real one bootstrapped
to 100%. If the volume keeps no 8.3 names, Tor windows show a clear error instead (§6).

### 3.6 Tests that were wrong at first
These were caught before any result was counted:
- **Cookie test hosts:** the first cookie test embedded `embedded.tor.test` in `secure.tor.test`, which
  are the same site, so the "third-party" cookie was first-party and the check was meaningless. It now
  embeds `cookies.example.net`, with a normal-window control showing the cookie is kept there when
  Settings allows it.
- **Worker test:** its service worker answered `respondWith()` with a string, not a `Response`.
- **New identity test:** it closes the window it is called from within about 25 ms, so the DevTools reply
  to the call was lost. The test presses the button without waiting for a reply.
- **Earlier tests:** the Stage 1 and 2 onion tests expected every onion input to be refused. Valid
  addresses now open Tor windows, so those tests now check only the inputs that must still be refused.

## 4. How it is tested
**The fake tor** (`tests/e2e/fake_tor.py`). Most tests run TorFox with a scripted tor, chosen with the
test-only `--torfox-test-tor-command`. It:
- reads the `torrc` TorFox wrote and implements the control port: PROTOCOLINFO, real SAFECOOKIE, TAKEOWNERSHIP, GETINFO, events, NEWNYM;
- follows a scripted bootstrap that can hold, die or ignore its owner;
- records every control command and SOCKS request.

**Two local web servers** stand in for the internet:
- **Direct:** every test URL points here. A Tor window must never reach it.
- **Tor side:** the fake tor relays every SOCKS connection here, whatever host it names.

Some tests put an https site with a trusted test certificate on the Tor side, for secure contexts.

`test_tor.py` covers, among others:
- **Waiting and routing:** waiting at 10% then loading through SOCKS with the host name (address type 3);
  the exact `torrc` and control command order; tor not started until it is needed.
- **Onion addresses:** valid ones open from a normal window; mistyped and mixed ones are refused and never
  searched; Tor-window searches go to the onion service.
- **Refusals:** local addresses as navigations and subresources; the link menu; camera, clipboard and
  `mailto:` in Tor windows; third-party cookies (with a control).
- **WebRTC and DNS:** no ICE candidates at all; NetLog shows no lookup that could leave the computer (with
  a control).
- **Fail closed and recovery:** tor dying (fail closed, restart, the waiting page loads); each fail-closed
  layer on its own; service worker requests.
- **Identity and lifetime:** new identity (old window closed, cookies gone, new context); the job object;
  the system proxy setting refused for the Tor context.
- **Settings:** bridges reaching `torrc`, restart on change, invalid lines refused, *Custom* without lines.
- **Profile paths:** non-ASCII profile folders.

**With the bundled tor** (`TORFOX_LIVE_TESTS=1`):
- `RealTorTest`: tor 0.4.9.16 accepts the configuration, is controlled, bootstraps (100% here), runs in a
  job and exits with TorFox.
- `LiveTorTest`: from a Tor window, `check.torproject.org/api/ip` reports `"IsTor": true`, and DuckDuckGo's
  onion service loads.

## 5. Mutation check
RESULTS_MUT_DETAIL

## 6. Not verified, and known gaps
- **Not Tor Browser.**
  - Sites can fingerprint TorFox.
  - Circuits are separated per destination address (`IsolateDestAddr`), not per first-party site:
    Chromium's SOCKS client has no authentication, so per-site credentials are impossible.
  - All Tor windows share one context, so a site can recognise you across Tor windows by its cookies until
    *New identity*.
- **macOS:** none of this has run on macOS. The bundle layout check covers the tor files only.
- **Bridges over the real network** have not been exercised, since this machine reaches Tor without them.
  What is tested is the `torrc` TorFox writes for each kind, with the scripted tor.
- **Not tested on their own:**
  - TorFox's `connected` state also requires the SOCKS port to have been applied. No test makes applying
    it fail.
  - WebSockets from Tor windows go through the proxy like everything else, but CEF does not show them to
    TorFox's guards. A WebSocket to a local address is therefore not refused by TorFox: it goes to tor,
    whose exits refuse private addresses. That behaviour of tor is not tested here.
- **No 8.3 names:** on a volume that keeps no short names, a profile folder whose name tor cannot read
  makes Tor windows unavailable, with a message saying why.
- **Downloads in Tor windows** use the Tor context like every other request, but no test downloads a file
  in a Tor window.
- **Custom bridges are stored in plain text** in `settings.json`, like every setting.
