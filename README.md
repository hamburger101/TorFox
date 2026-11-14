# TorFox

A privacy-focused desktop web browser for Windows 10/11 (x64) and macOS (Apple Silicon and Intel),
built on the Chromium Embedded Framework (CEF), with built-in `.onion` browsing through a bundled
official Tor client.

> **Status: early development (Stage 0, feasibility). Not usable yet.** Nothing in this repository should
> be relied on for security or anonymity until [`docs/FEATURE_STATUS.md`](docs/FEATURE_STATUS.md) marks it
> *Verified by test*. TorFox is **not** Tor Browser and does not claim Tor Browser's anti-fingerprinting
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
| `deps.lock.json` | Pinned, hash-verified third-party binaries (CEF, Tor) |
| `scripts/fetch_deps.py` | Downloads and verifies dependencies; refuses unverified files |
| `cmake/` | Build helpers |
| `config/release.json` | Updater repository and public key; never secrets |
| `spikes/` | Stage 0 feasibility experiments (not shipped) |
| `tests/` | Automated tests |
| `docs/` | Architecture, security and status documentation |

## Building (Windows, development)
```
python scripts/fetch_deps.py cef windows64-standard
cmake -S . -B out/spike -G "Visual Studio 18 2026" -A x64 -DTORFOX_BUILD_SPIKES=ON
cmake --build out/spike --config Release --target cefspike
```
Use the CMake bundled with Visual Studio. The browser itself is not buildable yet; see the staged plan in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## License
[Mozilla Public License 2.0](LICENSE). Third-party components keep their own licenses.
