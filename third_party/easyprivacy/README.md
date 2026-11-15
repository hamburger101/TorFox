# EasyPrivacy tracker domains

`domains.txt` is the list TorFox blocks third-party requests to (Settings > Privacy & Security >
*Block trackers*). It is derived from **EasyPrivacy** by The EasyList authors
(<https://easylist.to/>, source: <https://github.com/easylist/easylist>), at the commit pinned in
`sources.lock.json`.

EasyPrivacy is dual-licensed under the GNU General Public License v3 (or later) and the Creative Commons
Attribution-ShareAlike 3.0 Unported licence (<https://easylist.to/pages/licence.html>). TorFox uses it
under **CC BY-SA 3.0**. `domains.txt` is an adaptation (whole-domain rules only) and is distributed under
the same licence. It is data, kept separate from TorFox's MPL-2.0 code.

How the list is made (`scripts/update_trackers.py`):
- every EasyPrivacy source file at the pinned commit is fetched from GitHub and checked against the SHA-256
  values in `sources.lock.json`;
- only whole-domain rules are kept (`||example.com^`, optionally with `$third-party` or resource types);
  rules limited to certain sites, to first-party requests, or with paths are left out;
- whole-domain exceptions (`@@||example.com^`) are removed.

The build (`scripts/gen_trackers.py`) refuses to use `domains.txt` if its SHA-256 differs from the lock.
