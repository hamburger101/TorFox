# Stage 9 findings: security review and hardening

Stage 9 is the security review of the code that matters most: the bridge and the `torfox://` scheme, the
Tor integration, and the updater with its release workflow. It adds tests where the review found a claim
without one, fixes what the review and those tests found, and brings the documentation up to date. It
also builds the address-bar suggestions that Stage 6 left out (§5). Results:

- **Unit tests:** 181 tests: 180 pass and 1 is skipped (`ClearData.NeverFollowsLinks`, which needs the right to make symbolic links; CI has it).
- **End-to-end tests (Windows):** 246 tests: 243 pass and 3 are skipped (the live tests, which need the Tor network: `TORFOX_LIVE_TESTS=1`).
- **Platform tests (Windows):** release build 3 of 3 pass; per-user installer 5 of 5 pass, including the update over a stale `dxil.dll`. The 5 per-machine installer tests are skipped here: they need administrator rights, which CI has.
- **Release tooling tests:** 6 of 6 pass.
- **Mutation check:** 19 of 19 detected (§4).

## 1. What the review found, and what changed
Two reviews: one of the updater, the installer hand-off and the release workflow; one of the bridge, the
internal scheme and Tor. The second found no way for a web page to reach `torfox://` or the bridge. Every
finding below was fixed unless it says otherwise. Each code fix has a test that the mutation check (§4)
shows fails without it. The release workflow's changes (§1.5) are checked by tests of its structure, not
by mutations.

### 1.1 Tor windows: WebRTC looked names up outside Tor (found by a new test)
The review listed this as unverified, so Stage 9 wrote the test first
(`test_tor.TorDnsTest.test_webrtc_looks_up_no_name_for_tor_windows`). A page in a Tor window created an
`RTCPeerConnection` with a STUN and a TURN server given by name, and added a remote candidate given by
name. Chromium's NetLog showed `turn.<name>` and `peer.<name>` **looked up on this computer's resolver**: a
DNS leak of names the page chose. (STUN was not looked up, because Tor windows allow WebRTC no UDP.)

Tor windows now have no WebRTC at all, as in Tor Browser. The renderer removes `RTCPeerConnection`,
`webkitRTCPeerConnection` and the related interfaces from every frame of a Tor window before the frame's
scripts run (`RendererApp::OnContextCreated`, for browsers created with the `torfox.tor` flag). The test
now finds no lookup of the Tor window's names, while the same page in a normal window is still looked up
(the control). `test_tor_windows_have_no_webrtc` checks the page itself, an `about:blank` frame, a
`srcdoc` frame, and a frame of another site running in another renderer process. Normal windows keep
WebRTC everywhere.

`RTCDataChannel` stays visible in Tor windows. Blink adds it after TorFox has seen the frame. It cannot be
constructed (the test checks that), and only an `RTCPeerConnection` can make one.

### 1.2 New identity left the old Tor context able to send (medium)
*New identity* closed the Tor windows and sent `SIGNAL NEWNYM`, but the old context stayed usable until
every window had closed, and its own gate let requests through while tor was connected. A service worker
outlives its window: it could keep fetching after NEWNYM, over the new circuits, and so link the old
identity to the new one.

The old context is now **retired** before NEWNYM (`ContextManager::DropTor`):
- its windows' guards and its own gate refuse every request ("tor identity retired");
- its proxy is moved to the placeholder port where tor is not, so even a request no guard sees (a
  WebSocket) cannot reach tor.

`TorContextGateTest.test_after_new_identity_the_old_context_sends_nothing` starts a service worker that
fetches and opens WebSockets every 150 ms. The probe works while the context is current. After *New
identity*, the worker keeps trying and is refused, and neither the site nor tor sees another request.

### 1.3 `x.onion.` escaped the resolver rule (medium)
`MAP *.onion ~NOTFOUND` keeps `.onion` names away from DNS for the one kind of request TorFox's guards
never see: WebSockets. `x.onion.` (the same name with a trailing dot) does not match `*.onion`. The rules
are now `MAP *.onion ~NOTFOUND,MAP *.onion. ~NOTFOUND`. The onion resolver tests add a WebSocket and an
image to the trailing-dot name, with the guards on and with them off.

### 1.4 The updater could start a different file than it checked (medium)
TorFox hashed the downloaded setup through an open handle but started it by its path. A junction made
between the check and the start (the `Updates` folder replaced by a link elsewhere) would have started
whatever the link pointed to. TorFox now asks Windows for the real path of the file it holds open
(`GetFinalPathNameByHandleW`) and starts the setup by that path, still holding the handle. Windows refuses
to rename the file, or any folder above it, while a handle without `FILE_SHARE_DELETE` is open (checked
here: error 5). In `test_updates.UpdateTest.test_the_installer_is_started_by_its_real_path`, another
program turns the `Updates` folder into a junction to a folder of its own while TorFox runs. The setup
that runs is the checked file, started by its real path and never through the junction. The test cannot
swap the junction in the instant between the check and the start, so it shows the path TorFox uses, not
the race itself. See §2.1 for what remains.

### 1.5 The signing key was more exposed than the documentation said (medium)
The release workflow's one publish job held the secret key while it also handled every build output, and
its actions were referenced by tag. Now:
- the key is a secret of a `release` environment with required reviewers and a `v*` tag rule;
- only the **sign** job can read it. That job receives the manifest and nothing else, and returns only
  the signature;
- a tag whose commit is not on `main` is refused;
- every action is pinned by commit;
- preflight no longer needs the secret.

`tests/tools/test_release.py` checks the workflow's structure: the jobs, which job uses the secret, the
pins and the `main` check. The environment's reviewers and tag rule are GitHub settings that no test here
can see. [`RELEASING.md`](RELEASING.md) says how to set them.

### 1.6 Smaller fixes (low)
- **A replayed older release looked like "up to date".** A signed release older than one TorFox had seen
  was ignored, but silently. It is now reported as an error
  (`test_an_older_signed_release_cannot_be_replayed`).
- **A failure to save the record of seen releases was silent.** Without that record (`update.json`), a
  restart would forget the anti-replay timestamp. TorFox now offers no update until it can save it
  (`test_no_update_is_offered_while_the_record_cannot_be_saved`).
- **An update could leave an old optional file behind.** `dxil.dll` is shipped only when the build has it.
  The setup now deletes it first (`[InstallDelete]`), and the update test plants a stale copy. Any file
  a future version stops shipping must be added there too.
- **After tor stopped, its old SOCKS port stayed in use.** The Tor context kept pointing at the port, where
  another program could then listen and be sent the names Tor windows open. Once tor stops, the context
  points at the placeholder port (`ProxyLayerAfterDeathTest.test_a_dead_tors_port_is_never_used_again`
  listens on the old port and receives nothing).
- **A command-line switch could remove CEF's own switch.** The policy erased every switch the user named,
  including CEF's own switch of that name. With `--log-severity`, that would have undone CEF's "no logs" in
  release builds. Now only the user's own switch goes, and release builds always get
  `log-severity=disable` (unit tests; not checked end to end in a release build).

### 1.7 Chromium's DNS probe looked up google.com (found by a new test)
After a page fails with "name not resolved", Chromium looks up `google.com` itself, to tell the user
whether DNS works. Normal and private windows did this. It is a lookup, not a connection, but it is
traffic TorFox did not ask for. `alternate_error_pages.enabled` is now off in every context, which
stops the probe. `test_quiet_startup.FailedLookupTest` checks that a failed page makes no lookup but its
own. Tor windows never looked anything up.

### 1.8 Checked and found fine
- **Private downloads do not record their address.** Chromium writes the "downloaded from the internet"
  mark with `HostUrl=about:internet` for off-the-record downloads.
  `test_private_downloads_keep_the_mark_but_not_the_address` checks it in a private window. Tor windows
  are off the record too but are not tested separately.
- **Site isolation.** The new `test_site_isolation.py` shows that a frame of another site runs in its own
  renderer process, while a frame of the same site does not (the control). It covers normal and private
  windows. Tor windows are not covered.

## 2. Remaining risks
### 2.1 The update's elevated stage
TorFox holds the checked setup open until it starts it (§1.4). Inno's setup then unpacks its second stage
into the user's temporary folder, and only that stage asks for administrator rights. A program already
running as the user could change it in between, and the user would approve its UAC prompt. Windows does
not treat this as a security boundary, but it is a way from "runs as you" to "runs as administrator" that
TorFox's own check does not close. Closing it needs a helper that runs as administrator and checks the
setup itself. TorFox does not have one. See [`SECURITY.md`](SECURITY.md).

### 2.2 Others
- **Withheld updates.** Signed releases do not expire, so someone who can answer for GitHub can hide newer
  releases (but not install older or unsigned ones).
- **The placeholder port.** A retired or orphaned Tor context points at port 9 on loopback. TorFox's guards
  refuse every request but WebSockets first, but a program listening on port 9 would be sent a WebSocket's
  host name.
- **WebSockets are not seen by TorFox's request guards** (CEF does not show them). Tor windows' WebSockets
  go through tor like everything else, and the resolver rules keep `.onion` names from DNS.
- **macOS** has never been built or run. Everything above is verified on Windows only.

## 3. How it is tested
New and changed tests:

| Test | Shows |
|---|---|
| `test_tor.TorDnsTest.test_webrtc_looks_up_no_name_for_tor_windows` | No WebRTC name of a Tor window reaches the resolver (control: a normal window's do) |
| `test_tor.TorContextIsolationTest.test_tor_windows_have_no_webrtc` | No `RTCPeerConnection` in any frame of a Tor window (control: a normal window) |
| `test_tor.TorContextGateTest.test_after_new_identity_the_old_context_sends_nothing` | A retired context's worker sends nothing (control: it did before) |
| `test_tor.ProxyLayerAfterDeathTest.test_a_dead_tors_port_is_never_used_again` | Nothing goes to tor's old port once tor has stopped |
| `test_quiet_startup.OnionResolverTest` (both tests) | `x.onion.` stays away from DNS too |
| `test_quiet_startup.FailedLookupTest` | A failed page makes no lookup but its own |
| `test_downloads.DownloadTest.test_private_downloads_keep_the_mark_but_not_the_address` | Private downloads keep `ZoneId=3` and no address |
| `test_site_isolation.SiteIsolationTest` (2 tests) | Other sites' frames run in other processes |
| `test_updates.UpdateTest`: the real-path, replay and save-failure tests | §1.4 and §1.6 |
| `tests/platform/test_installer_win.py` (update test) | A stale optional file is removed |
| `tests/tools/test_release.py` | Preflight without the secret, and the workflow's jobs, secret use and pins |
| `test_suggestions.SuggestionsTest` (6 tests) | §5: the list, keyboard and mouse, hiding, what the list's page may do, closing with the window |
| `test_tor.TorSuggestTest` | §5: Tor windows suggest bookmarks only (control: the normal window also suggests history) |
| Unit: `CmdlinePolicy.TheUsersSwitchNeverRemovesCefsOwn`, `TestBuildsLeaveLoggingToCef`; `Release` replay flag | §1.6 |
| Unit: `PlacesSuggest.*`, `NavInput.InternalPages`, `InternalUrl.KnownPages`, `BridgeValidator.*` (suggestion commands), `Guards.*` (the list's role) | §5 |

## 4. Mutation check
Run on the Stage 9 commit (`02cd9c9`) in a separate worktree. Each mutation removes one protection, and
every check listed for it must then fail. All 19 were detected, each by every one of its checks:

- **Tor:**
  - `tor-webrtc-kept`: WebRTC left in Tor windows (2 checks);
  - `tor-retired-context-not-refused`, `tor-retired-context-keeps-proxy`: each half of retiring the old
    context on *New identity*;
  - `tor-new-identity-keeps-the-context`: *New identity* without retiring and replacing the context at
    all (rewritten for the new code);
  - `tor-dead-port-kept`: the context left on a stopped tor's port;
  - `onion-trailing-dot-resolvable`: the resolver rule for `x.onion.` (2 checks).
- **Network and engine:**
  - `dns-probe-left-on`: Chromium's DNS probe;
  - `site-isolation-off`: `--site-per-process` replaced by its opposite (2 checks);
  - `cmdline-erases-cef-switches`, `cmdline-release-logging-not-forced`: the command-line policy (unit
    tests).
- **Updates and setup:**
  - `update-started-through-links`: the setup started by its path instead of its real path;
  - `update-replay-shown-as-up-to-date` (2 checks), `update-record-save-failure-ignored`;
  - `installer-keeps-stale-files`: the setup's `[InstallDelete]` entry (the per-user update test).
- **Suggestions and the bridge:**
  - `suggest-history-in-tor-windows`: history suggested in Tor windows;
  - `suggest-page-in-tabs`: the list's page allowed in tabs (2 checks: unit and end to end);
  - `suggest-kept-on-tab-switch`, `suggest-list-not-closed`: hiding and closing the list;
  - `bridge-page-allowlist`: the bridge's page-and-role check (2 checks; rewritten for the new role).

Two earlier mutations no longer matched the code after Stage 9 changed it (`tor-new-identity-keeps-the-context`
and `bridge-page-allowlist`). Both were rewritten and are among the 19. Every other mutation still applies
to the code, but only these 19 were run on this commit. The rest last ran on the commits named in their
stages' findings.

## 5. Address-bar suggestions
Stage 6 left these out, because the list has to cover the top of the page and CEF had no pop-up for it
(Stage 6 findings §2). Stage 9 builds the list as a CEF overlay of the window (`CefWindow::AddOverlayView`)
holding a browser of its own, `torfox://suggest/`.
- **Where suggestions come from.** Only `Places::Suggest`: bookmarks first, then history, matched against
  the start of the address (ignoring `http(s)://` and `www.`) and then anywhere in the title or address.
  Up to six are shown. Nothing is sent to DuckDuckGo or anywhere else while typing.
- **Tor windows** get bookmarks only, so that pages visited outside Tor never show up in a Tor window
  (`test_tor.TorSuggestTest`, with the normal window as the control). Private windows get bookmarks and
  history, like the normal window they are opened from. They record no history of their own.
- **The list's page has the least it needs.** It has a role of its own in the bridge (`suggest`). Its
  commands are `suggest.subscribe`, to receive what the browser shows, and `suggest.open`, which names a
  row by its position (0 to 5), never an address. It cannot navigate, open tabs, read bookmarks or
  history, or ask for other suggestions. Its page cannot be opened in a tab, typed or otherwise: the
  address bar refuses `torfox://suggest/` (`IsTabPage`), and the guards keep the page to its own browser.
  Its overlay never takes the focus, so typing stays in the address bar.
- **Keyboard and mouse.** The arrow keys move through the rows. Enter opens the highlighted row, or what
  was typed if none is. A click on a row opens it in the active tab. Escape closes the list and keeps the
  text; a second Escape restores the address. Leaving the address bar and switching tabs hide it, and it
  closes with its window. Resizing the window hides it too, but no test checks that.
- **Found while testing.** A typed `torfox://suggest/` was first classified as a TorFox page and loaded in
  a tab, where the navigation guard then refused it. Nothing loaded, but the tab kept that address. The
  address bar now refuses it outright. Escape pressed while the first query's reply was still on its way
  did not close the list. The shell now treats a pending query as an open list and drops replies to
  queries it has abandoned.

Not covered: display scales other than this computer's (the overlay is placed in window coordinates
computed from the shell's CSS pixels), screen readers, and macOS.
