# EasyList ad-blocking rules

`filters.txt` holds the rules TorFox's ad blocker uses (Settings > Privacy & Security > *Block ads*). It is
derived from **EasyList** by The EasyList authors (<https://easylist.to/>, source:
<https://github.com/easylist/easylist>): the files EasyList's own build template (`easylist.template`)
puts into `easylist.txt`, at the commit pinned in `sources.lock.json`.

EasyList is dual-licensed under the GNU General Public License v3 (or later) and the Creative Commons
Attribution-ShareAlike 3.0 Unported licence (<https://easylist.to/pages/licence.html>). TorFox uses it
under **CC BY-SA 3.0**. `filters.txt` is an adaptation (some rules left out, see below; the rest sorted)
and is distributed under the same licence. It is data, kept separate from TorFox's MPL-2.0 code.

How the list is made (`scripts/update_adblock.py`):
- every source file at the pinned commit is fetched from GitHub (and only GitHub) and checked against the
  SHA-256 values in `sources.lock.json`;
- comments are dropped, and so are rules in syntax TorFox does not implement: regular expressions,
  procedural element hiding (`:has-text()`, `:-abp-...`, `#?#`), scriptlets and snippets (`##+js`, `#$#`),
  HTML filtering (`##^`) and options that rewrite a request instead of blocking it (`$csp`, `$redirect`,
  `$removeparam`, ...);
- the remaining rules are sorted and written once each.

The build (`scripts/gen_adblock.py`) refuses to use `filters.txt` if its SHA-256 differs from the lock.
TorFox never downloads lists itself: a new list comes with a new TorFox version.
