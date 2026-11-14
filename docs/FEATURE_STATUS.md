# Feature status

A feature counts as **Verified** only when an automated test that checks its behaviour (not just its UI)
passes on the platform listed. An interface existing is not evidence.

Legend: ✅ Verified by test · 🟡 Implemented, not verified · ⬜ Not started · ⛔ Not supported (with reason)

| Area | Feature | Windows | macOS | Evidence |
|---|---|---|---|---|
| Engine | Sandbox on for every process type (incl. network service) | ⬜ | ⬜ | Spike A only (not product code) |
| Engine | Site isolation | ⬜ | ⬜ | |
| UI isolation | `torfox://` unreachable from web content | ⬜ | ⬜ | |
| Browsing | Tabs, navigation, bookmarks, history | ⬜ | ⬜ | |
| Search | DuckDuckGo default; onion input never searched | ⬜ | ⬜ | |
| Downloads | Ask before download (default ON) | ⬜ | ⬜ | |
| Downloads | Disable downloads blocks every vector | ⬜ | ⬜ | |
| Privacy | Tracker blocking | ⬜ | ⬜ | |
| Privacy | Third-party cookie blocking | ⬜ | ⬜ | |
| Privacy | HTTPS-first | ⬜ | ⬜ | |
| Privacy | Pop-up blocking | ⬜ | ⬜ | |
| Privacy | No telemetry / no unrequested network traffic | ⬜ | ⬜ | Spike C found an unrequested `www.google.com` connection; not fixed |
| Privacy | Private browsing | ⬜ | ⬜ | |
| Privacy | Clear data on exit | ⬜ | ⬜ | |
| Permissions | Camera, mic, location, notifications, clipboard, pop-ups | ⬜ | ⬜ | |
| Network | WebRTC leak protection | ⬜ | ⬜ | Spike D only |
| Tor | Bundled tor managed automatically | ⬜ | ⬜ | |
| Tor | Onion traffic only via Tor; no DNS leak; fail closed | ⬜ | ⬜ | Spike C only |
| Tor | Status indicator | ⬜ | ⬜ | |
| Settings | All sections persist across restart | ⬜ | ⬜ | |
| Updates | Signed GitHub Releases updates | ⬜ | ⬜ | |
| Packaging | Windows installer | ⬜ | — | |
| Packaging | macOS DMG | — | ⬜ | |
