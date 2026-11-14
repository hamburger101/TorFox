# TorFox

A privacy-focused desktop web browser for Windows 10/11 (x64) and macOS (Apple Silicon and Intel),
built on the Chromium Embedded Framework (CEF), with built-in `.onion` browsing through a bundled
official Tor client.

> **Status: early development. Stage 1 (core shell) is done on Windows. Not usable for browsing yet.**
> Nothing in this repository should be relied on for security or anonymity until
> [`docs/FEATURE_STATUS.md`](docs/FEATURE_STATUS.md) marks it *Verified by test*. The macOS build has
> not been compiled yet. TorFox is **not** Tor Browser and does not claim Tor Browser's anti-fingerprinting
> or anonymity protections.

## Goals
- DuckDuckGo as the default search engine. Search queries are never sent to any other provider.
- `.onion` sites open in normal TorFox tabs. Tor traffic is routed through Tor, isolated from regular
  browsing, and fails closed.
- Download protection: *Ask before download* (on by default) and *Disable downloads* (blocks everything).
- Chromium's sandbox and site isolation, tracker blocking, third-party cookie blocking, HTTPS-first,
  pop-up blocking, and no telemetry.
- Signed automatic updates from GitHub Releases.

## Repository layout
| Path | Contents |
|---|---|
| `src/core/` | Pure C++ logic with no CEF dependency (settings, URL guards, bridge validation, command-line policy), unit-tested |
| `src/browser/` | The CEF application: windows, tabs, request contexts, the `torfox://` scheme, the bridge, platform code |
| `ui/` | The TorFox interface (TypeScript + Lit, built by Vite), compiled into the binary |
| `tests/unit/`, `tests/e2e/`, `tests/platform/` | Unit, end-to-end, release-build platform tests, and the mutation check |
| `deps.lock.json`, `scripts/fetch_deps.py` | Pinned, hash-verified third-party binaries (CEF, Tor); unverified files are refused |
| `config/release.json` | Updater repository and public key; never secrets |
| `spikes/` | Stage 0 feasibility experiments (not shipped) |
| `docs/` | Architecture, security, testing and status documentation |

## Building (Windows)
Needs Visual Studio 2026 with desktop C++, Node.js 22.12 or newer, and Python 3.13 or newer. Use the
CMake bundled with Visual Studio.
```
python scripts/fetch_deps.py cef windows64-standard
python scripts/fetch_deps.py nlohmann_json header
python scripts/fetch_deps.py googletest source
npm --prefix ui ci
cmake -S . -B out/dev -G "Visual Studio 18 2026" -A x64
cmake --build out/dev --config Release --parallel
out\dev\src\browser\Release\TorFox.exe
```
Release builds only: CEF's Debug build asserts in off-the-record profiles. For the test build and how
to run the tests, see [`docs/TESTING.md`](docs/TESTING.md). For what Stage 1 found and what it leaves open,
see [`docs/STAGE1_FINDINGS.md`](docs/STAGE1_FINDINGS.md). To report a vulnerability, see
[`docs/SECURITY.md`](docs/SECURITY.md).

## License
[Mozilla Public License 2.0](LICENSE). Third-party components keep their own licenses.
