# Stage 11 findings: the built-in ad blocker

Stage 11 adds an ad blocker to TorFox, in the spirit of uBlock Origin: ad requests are cancelled before
they are sent, ad boxes are hidden and ad pop-ups are blocked, with the EasyList rules. It works in every
window, Tor windows included. A shield next to the address bar shows what was blocked on the page and can
allow ads on a site. *Settings → Privacy & Security → Block ads* turns it off.

**Results, on Windows 11 with the final code:**

| Suite | Result |
|---|---|
| Unit tests | 216 run: 215 pass, 1 skipped (as before); 22 of them are the ad blocker's (`AdBlock.*`) |
| E2E, whole suite (with `test_adblock`) | 293 run: 287 pass, 3 skipped (live Tor), 1 expected failure (ad WebSockets), 2 failed, then passed when run again (below) |
| Platform tests (release build, both setups) | 18 of 18 pass |
| Mutation check | 16 of 16 caught (§7) |

- **The two failures in the whole-suite run:** `test_settings`'s persistence test did not know the new
  string setting `privacy.adsAllowedSites` (the test now gives it a value; it passes). `test_memory`'s
  section test timed out waiting for the Memory section while the release build was compiling alongside;
  it passed when run again. Neither was run a second time inside a whole-suite run.
- **macOS** was not built or tested.

## 1. The engine and the list
TorFox has its own filter engine (`src/core/adblock/`), not a copy of uBlock Origin or of a browser
extension: TorFox's browsers (CEF, Alloy style) run no browser extensions, and uBlock Origin itself needs an
extension API (Manifest V2's blocking `webRequest`) that Chromium is removing. The engine reads the Adblock Plus rule
syntax that EasyList is written in:

- request rules: `||host^`, `|`, `*`, `^`, `@@` exceptions; the type options (script, image, stylesheet,
  object, xmlhttprequest, subdocument, ping, websocket, font, media, other, popup, and their `~` forms);
  `$third-party` / `$first-party`, `$domain=`, `$match-case`, `$important`; and on exceptions
  `$document`, `$elemhide`, `$generichide` and `$genericblock`;
- element hiding: `##selector`, `domains##selector` and `#@#` exceptions, with `~domain` exclusions and
  `name.*` domains.

**A rule TorFox does not fully understand is never applied**, so a rule never applies more broadly than its
authors meant. Ignored: regular-expression rules, procedural element hiding (`:has-text()`, `:-abp-…`,
`#?#`), scriptlets (`##+js`, `#$#`), HTML filtering, and options that rewrite a request instead of blocking
it (`$csp`, `$redirect`, `$removeparam`). This is the main difference from uBlock Origin, which implements
those and ships its own lists that use them; TorFox blocks less on some sites (ads that only uBlock
Origin's scriptlets or its own lists stop). **Blocking rules with `$document` are ignored
too: TorFox never blocks a page the user opens**, it only blocks what a page loads.

Element-hiding selectors are checked before use: one that could close the style rule it is put in, open a
comment or leave a string unterminated is ignored (`AdBlock.SelectorsThatCouldEscapeTheirRuleAreIgnored`).
A list rule can therefore only ever hide elements, never add styles of its own.

**The list.** EasyList is compiled into TorFox (`third_party/easylist/filters.txt`, CC BY-SA 3.0, noted in
`THIRD_PARTY_NOTICES.txt`). `scripts/update_adblock.py` fetches the files EasyList's own build template
uses, from GitHub only, at a pinned commit (`fc9eb586`), and checks each file's SHA-256 against
`sources.lock.json`. The build (`scripts/gen_adblock.py`) refuses a `filters.txt` whose SHA-256 differs
from the lock. **TorFox never downloads filter lists**: a new list comes with a new, signed TorFox version.
This is slower than uBlock Origin's daily list updates, and is deliberate: nothing a list server sends can
change what TorFox does between releases, and the ad blocker contacts no one.

At the pinned commit the engine keeps **52,713 request rules and 24,426 hiding rules**, and ignores 608
rules, 565 of them `$document` blocking rules (`adblock_ready` in the test build's log). It is built on a
background thread at start-up: about **31 ms and 7.9 MB**. A check takes about 1 µs (rules
are indexed by a token of their pattern, as uBlock Origin and Adblock Plus do).

## 2. Where it applies
Every request a web page makes, except the page itself (main-frame requests are never checked), goes
through `content_filter::Check`, which tracker blocking now shares. A request is checked with:

- its **initiator**, the document that made it (for `$third-party` and `$domain=`), which CEF gives only to
  `GetResourceRequestHandler`; a small per-request handler keeps it;
- the tab's **top-level page**, for the shield and the sites with ads allowed.

### 2.1 Service workers bypassed tracker and ad blocking (found and fixed)
The first worker test showed that a service worker's own `fetch()` reached a listed ad server, **and a
listed tracker**, unchecked. It also did so in a private window. CEF does not pass a request that no tab
makes (service and shared workers) to the tab's handler. It passes it to the request context's handler,
which TorFox did not use for this. Tor windows were not affected for the network path, because their
context handler already held requests back while tor is not connected and always sends them through tor;
but the ad and tracker rules did not apply there either.

Each web context now checks these requests (`content_filter::ContextRequestHandler`), after the Tor gate
for Tor windows. Chromium gives a worker request only the site as its first party
(`https://example.com/`), so the request's top-level page is its initiator when that is on the same site,
and the site otherwise. The updater's private context is not a web context and is not filtered.

Tests: `test_adblock.WorkerTest` (normal and private windows, a site with ads allowed applying to its
worker), `test_tor.AdBlockTorWorkerTest` (a Tor window's service worker, through tor). Tracker blocking had
the same gap since Stage 4; `test_privacy` never tested workers.

A worker's requests in a **private or Tor window always have ads blocked**, even on a site that window
allowed ads on: the context does not know which window a worker belongs to. A site that needs its ads to
work in a private window may therefore still break; it is the strict side.

### 2.2 WebSockets (known gap)
An ad WebSocket is not blocked. CEF never passes WebSocket handshakes to `OnBeforeResourceLoad`, in a tab or
in a context; this is the gap tracker blocking already has ([`STAGE4_FINDINGS.md`](STAGE4_FINDINGS.md) §6,
[`STAGE1_FINDINGS.md`](STAGE1_FINDINGS.md) §5). `test_adblock.AdRequestTest.test_ad_websockets_are_blocked`
is an expected failure, so a CEF change that closes it shows up as an unexpected success. In Tor windows a
WebSocket still goes through tor (the proxy, not the guard, carries it).

### 2.3 Pop-ups
`$popup` rules are checked in `OnBeforePopup` with the opener's frame as the document, even when the user
clicked: a click on a page often opens an ad pop-up the user never meant to open. TorFox shows a notice
("Ad pop-up blocked: …"). `AdPopupTest` opens an ordinary pop-up as its control.

### 2.4 Tor windows
Ads are blocked in Tor windows by default, as the user chose. A blocked request is never sent, not to tor
and not directly (`AdBlockTorTest` checks the fake SOCKS server and the direct fixture). See §6 for what
this means for fingerprinting.

## 3. Ad boxes (element hiding)
When a document starts loading, `OnLoadStart` gives it a constructed style sheet
(`document.adoptedStyleSheets`) with `display:none!important` for the selectors that apply:

- **every site's rules** (EasyList's generic hiding rules, about 520 KB of CSS) in top-level documents only,
  as uBlock Origin does by default. In frames they would hide parts of other sites' embedded pages;
- **a site's own rules** in every frame of that site.

`$elemhide` and `$generichide` exceptions switch them off for a page.

Limits:
- **A blocked image or frame is not collapsed.** It shows as a broken image (or an empty frame) unless a
  hiding rule also covers its box. uBlock Origin collapses them; TorFox would need to find the element that
  made the request, which CEF's request handler does not say. Not done; listed in `FEATURE_STATUS.md`.
- The style sheet is added in the page's main world, so a page can see it (it can read
  `document.adoptedStyleSheets`). That is one more way a site can tell ads are blocked; it can tell anyway
  from the ads that did not load. A page could also remove it, and its ads would then show; the request
  blocking does not depend on it.
- An ad box a script adds later is still hidden: the style sheet applies to the whole document.

## 4. Allowing ads on a site, and the shield
The shield shows the number of ads and trackers blocked on the page in the tab (reset when the tab opens a
new page, sent to the toolbar at most every 300 ms). Its menu says what happened and offers *Allow ads on
{site}* (which also covers the site's subdomains) or *Block ads on {site} again*, then reloads the tab.

- **Normal windows** keep the list in the setting `privacy.adsAllowedSites` (validated: site names only,
  at most 1,000; an invalid list is refused, not saved).
- **Each private or Tor window** keeps its own list in memory; it is gone when the window closes and is
  never written to disk (`PrivateWindowAdBlockTest`, `AdBlockTorTest` check that the setting stays empty).

**A privacy trade-off, stated plainly.** Stage 4 refused per-site pop-up permissions because a list of sites
is a record of browsing. The ad allow list is such a record for normal windows: it is kept because uBlock
Origin's own users rely on one (a site that refuses to work with an ad blocker), and because it only ever
holds sites the user added by hand. Private and Tor windows never write it. Clearing it: *Settings →
Privacy & Security → Sites with ads allowed*.

## 5. Cost
Measured on a test page with 4,000 elements (the test build, several runs each):

| | *Block ads* off | *Block ads* on |
|---|---|---|
| Page load (navigation start to load event) | about 46 ms | about 46 ms |
| Style recalculation | 27.2–27.6 ms | 27.3–27.5 ms |
| Renderer memory per tab | 43.7–45.3 MB | 48.3–48.7 MB |

The cost is the generic hiding style sheet, parsed once per top-level page (about 4 MB per tab). Pages with
ads load faster with the blocker on, because the ads are not fetched; that was not measured.

## 6. Privacy and fingerprinting
- A site can tell that ads are blocked (the label says so), and which of its ads did not load.
- **In Tor windows this makes TorFox users more distinguishable.** Tor Browser blocks no ads, so that every
  user looks the same to a site; TorFox users with *Block ads* on look different from Tor Browser users.
  TorFox is not Tor Browser and makes no fingerprinting claims ([`SECURITY.md`](SECURITY.md)), but this is a
  new difference, so it is written down. *Block ads* applies to every window; turn it off if this matters.
- The ad blocker sends nothing anywhere: no list downloads, no counts reported. The quiet-startup test still
  passes with it on.

## 7. Mutation check
Sixteen mutations, each a small sabotage of the ad blocker that a test must catch
(`tests/e2e/run_mutations.py`): fifteen new ones, and `trackers-not-blocked` moved to the shared filter.

**All 16 were caught**: for each one, every listed check failed.

| Mutation | What it breaks | Checks | Result |
|---|---|---|---|
| `trackers-not-blocked` | the shared filter lets listed trackers through | 1 | caught by all |
| `ads-not-blocked` | the shared filter lets ad requests through | 2 | caught by all |
| `ads-not-blocked-in-tor-windows` | Tor windows' tabs skip the filter | 1 | caught by all |
| `ads-pages-checked` | main-frame requests are checked like any other | 1 | caught by all |
| `ads-third-party-option-ignored` | `$third-party` and `$first-party` are ignored | 2 | caught by all |
| `ads-setting-ignored` | *Block ads* off still blocks | 1 | caught by all |
| `ads-allowed-sites-ignored` | sites with ads allowed are still blocked | 2 | caught by all |
| `ads-private-windows-share-the-normal-list` | private windows use the saved list | 1 | caught by all |
| `ads-private-windows-save-their-list` | private and Tor windows write their list to the setting | 2 | caught by all |
| `ads-allowed-sites-not-validated` | any text is accepted as the list | 2 | caught by all |
| `ad-popups-not-blocked` | `$popup` rules never apply | 1 | caught by all |
| `ad-boxes-not-hidden` | the hiding style sheet is empty | 2 | caught by all |
| `ad-generic-hiding-in-frames` | every site's rules are given to frames too | 1 | caught by all |
| `ad-unsafe-selectors-used` | selectors that could escape their style rule are used | 1 | caught by all |
| `worker-requests-not-filtered-in-normal-windows` | the normal context does not check worker requests | 1 | caught by all |
| `worker-requests-not-filtered-in-private-and-tor-windows` | private and Tor contexts only run the Tor gate | 2 | caught by all |

## 8. Not verified
- **macOS** was not built or tested. The ad blocker is plain C++ and CEF; nothing in it is Windows-only.
- **Real sites.** The tests use local sites with the host names real EasyList rules name (adnxs.com,
  adsterra.com, popads.net, a site's own `##` rule). How much of a real site's advertising disappears depends
  on EasyList and on the site; it was not measured, and ads that need uBlock Origin's scriptlets or its
  own lists are not blocked.
- **Web compatibility.** A site may break with its ads blocked. EasyList's exceptions handle the known cases;
  others need *Allow ads on this site*.

## 9. The first signed release (v0.1.0)
Setting up signed releases (`scripts/make_release_key.py`, `docs/RELEASING.md`) and running the release
workflow for the first time found five problems. None was in a published release: the workflow publishes
nothing unless every build and test passes and a maintainer approves the signing.

- **macOS had never been compiled.** clang refused three things MSVC accepted: `BrowserWindow`'s implicit
  destructor with `TabClient` only declared, a `switch` without the Tor kind (`-Wswitch`), and a unit test
  that printed file times (gtest needs `std::format` for floating point, macOS 13.3+). Fixed.
- **macOS: the history and bookmarks database did not open.** It is opened with `SQLITE_OPEN_NOFOLLOW`,
  which refuses a path with a symbolic link anywhere in it. On macOS `/var` and `/tmp` are links, and a
  user's home folder may be one. TorFox now resolves the folder first; a link at the database file itself
  is still never followed (`PlacesFile.OpensThroughALinkedFolder`, `PlacesFile.ALinkAtTheDatabaseIsNotFollowed`,
  which need symbolic links and so run on macOS, not on Windows without the privilege).
- **macOS: the DMG's signature check failed.** Code signing allows only code in `Contents/Helpers`, and
  tor's licences, GeoIP files and `pt_config.json` were there. They are now in `Contents/Resources/tor`;
  CI checks that `Contents/Helpers` holds only code.
- **Windows: the release workflow did not recognise the runner's Visual Studio** (it reports "18", not
  "2026"). It now uses the major version, as CI did.
- **Windows: release builds can write a `debug.log` next to `TorFox.exe`.** `TorFox.exe` is CEF's bootstrap,
  which reads `--enable-logging` itself, before any TorFox code runs, and then writes its own start-up lines
  ("Client DLL resource config not found", "No installer config found") to `debug.log`. They name no page or
  site, and TorFox's own logging stays off (`--disable-logging` is forced in the browser). Without that
  switch nothing is written. Stage 10's test did not see this on the development machine because an old
  `debug.log` was already in the build folder; GitHub's fresh runner did. The test now allows that file
  only if every line in it is the bootstrap's and it names neither the test page nor its server. This is a
  known exception, not a fix: TorFox cannot stop CEF's bootstrap from reading its own command line.
- **Tests and the real update source.** With the release key in `config/release.json`, test builds would
  have checked the real GitHub. Test builds now leave the key out (`TORFOX_TEST_HOOKS`); the update tests
  keep using their fake GitHub.

Once while investigating, the release-build test's own check of the user's profile (set aside and put
back, compared byte for byte) reported that `User Data\chrome_debug.log` differed. The profile was checked
afterwards: no file in it had changed (all older than the test, and identical to a copy made before the
next runs). The `chrome_debug.log` did not appear again in two further runs. Its cause is not known.

