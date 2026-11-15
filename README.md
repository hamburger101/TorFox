# TorFox

A privacy-focused desktop web browser for Windows 10/11 (x64) and macOS (Apple silicon and Intel),
built on the Chromium Embedded Framework (CEF), with built-in `.onion` browsing through a bundled
official Tor client.

> **Status: pre-release. Every planned feature is built and tested on Windows. No release has been
> published yet, and the macOS build has never been compiled.**
> - Rely on a protection only where [`docs/FEATURE_STATUS.md`](docs/FEATURE_STATUS.md) marks it
>   *Verified by test*.
> - macOS is built and tested only on GitHub Actions, which has not run yet.
> - Updates are switched off in every build until the maintainer makes the release key
>   ([`docs/RELEASING.md`](docs/RELEASING.md)).
> - TorFox is **not** Tor Browser and does not claim Tor Browser's anti-fingerprinting or anonymity
>   protections ([`docs/SECURITY.md`](docs/SECURITY.md)).

## Features
- **Search:** DuckDuckGo (or its HTML or Lite pages); in Tor windows, DuckDuckGo's onion service. Queries
  are never sent to any other provider.
- **Tor:** `.onion` addresses open in Tor windows through the bundled official tor, with optional bridges
  (obfs4, WebTunnel, Snowflake; not yet tried over the real network). Tor windows fail closed: without a working tor nothing loads, and nothing
  is ever resolved or connected outside Tor. As in Tor Browser, Tor windows have no WebRTC. *New identity*
  closes the Tor windows, and the old ones' context sends nothing more. Normal windows never load `.onion`
  addresses.
- **Address bar:** suggestions come from your bookmarks and history only, never from a search engine or
  the network. Tor windows suggest bookmarks only.
- **Download protection:** *Ask before downloading* (on by default) and *Disable downloads*, which blocks
  every kind of download.
- **Privacy:** tracker blocking, third-party cookie blocking, HTTPS-first, pop-up blocking, permissions
  denied unless allowed, WebRTC leak protection, clearing data on exit, private windows, and no telemetry
  (a test checks that TorFox contacts nothing on start-up).
- **Security:** Chromium's sandbox for every process and site isolation. The TorFox interface is isolated
  from websites, and release builds ignore command-line switches that would weaken them.
- **Settings:** eight sections, all saved: General, Search, Privacy & Security, Downloads, Tor, Appearance
  (dark, light or system theme), Updates, About.
- **Updates:** from this repository's GitHub Releases only, each checked against TorFox's own release
  signature before it is installed ([`docs/UPDATING.md`](docs/UPDATING.md)).

## Installing
See [`docs/INSTALL.md`](docs/INSTALL.md). On Windows, TorFox installs for you only and never needs
administrator rights. Files are not signed with Microsoft or Apple certificates, so Windows and macOS warn
on first install.

## Repository layout
| Path | Contents |
|---|---|
| `src/core/` | Pure C++ logic with no CEF dependency (settings, URL rules, privacy lists, Tor configuration and control protocol, update verification), unit-tested |
| `src/browser/` | The CEF application: windows, tabs, request contexts, the `torfox://` scheme, the bridge, Tor and update services, platform code |
| `ui/` | The TorFox interface (TypeScript + Lit, built by Vite), compiled into the binary |
| `tests/unit/`, `tests/e2e/`, `tests/platform/`, `tests/tools/` | Unit, end-to-end, release-build and installer, and release-tooling tests, and the mutation check |
| `deps.lock.json`, `scripts/fetch_deps.py` | Pinned, hash-verified third-party files (CEF, Tor, libsodium, Inno Setup, minisign, …); unverified files are refused |
| `config/release.json` | Updater repository and public key; never secrets |
| `packaging/` | Windows setup (Inno Setup), macOS disk image scripts, license texts |
| `.github/workflows/` | CI and the release workflow |
| `spikes/` | Stage 0 feasibility experiments (not shipped) |
| `docs/` | Architecture, security, installing, updating, releasing, testing, status, and each stage's findings |

## Building (Windows)
Needs Visual Studio 2026 with desktop C++, Node.js 22.12 or newer, and Python 3.13 or newer. Use the
CMake bundled with Visual Studio.
```
python scripts/fetch_deps.py cef windows64-standard
python scripts/fetch_deps.py nlohmann_json header
python scripts/fetch_deps.py googletest source
python scripts/fetch_deps.py sqlite source
python scripts/fetch_deps.py tor windows-x86_64
python scripts/fetch_deps.py libsodium windows-msvc
npm --prefix ui ci
cmake -S . -B out/dev -G "Visual Studio 18 2026" -A x64
cmake --build out/dev --config Release --parallel
out\dev\src\browser\Release\TorFox.exe
```
Release builds only: CEF's Debug build asserts in off-the-record profiles. To build the setup, run
`python scripts/package_windows.py` after the release build. For the test build and how to run the tests,
see [`docs/TESTING.md`](docs/TESTING.md). To report a vulnerability, see [`docs/SECURITY.md`](docs/SECURITY.md).

## License
[Mozilla Public License 2.0](LICENSE). Third-party components keep their own licenses
([`packaging/THIRD_PARTY_NOTICES.txt`](packaging/THIRD_PARTY_NOTICES.txt)).
