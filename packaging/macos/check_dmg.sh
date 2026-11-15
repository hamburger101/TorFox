#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Checks a TorFox DMG the way a user installs it (CI, macOS):
#   packaging/macos/check_dmg.sh <TorFox-<v>-mac-<arch>.dmg>
# mounts it read-only, checks its contents and the app's signature, copies
# the app out, and runs it until its renderer process has started.
# It uses the real per-user data folder (~/Library/Application Support/TorFox),
# so run it only on a CI machine.
set -eu

dmg=$1
mnt=$(mktemp -d)
dest=$(mktemp -d)
pid=""
cleanup() {
  if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
  hdiutil detach "$mnt" -quiet 2>/dev/null || true
  rm -rf "$dest"
}
trap cleanup EXIT

hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
test -d "$mnt/TorFox.app"
test -L "$mnt/Applications"
test -f "$mnt/TorFox.app/Contents/Resources/THIRD_PARTY_NOTICES.txt"
test -f "$mnt/TorFox.app/Contents/Resources/licenses/CEF-LICENSE.txt"
codesign --verify --deep --strict "$mnt/TorFox.app"
ditto "$mnt/TorFox.app" "$dest/TorFox.app"
hdiutil detach "$mnt" -quiet
codesign --verify --deep --strict "$dest/TorFox.app"

"$dest/TorFox.app/Contents/MacOS/TorFox" >/dev/null 2>&1 &
pid=$!
i=0
until pgrep -f "$dest/TorFox.app/.*TorFox Helper .Renderer." >/dev/null; do
  i=$((i + 1))
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "TorFox exited before a renderer started" >&2
    exit 1
  fi
  if [ "$i" -gt 90 ]; then
    echo "no renderer process after 90 s" >&2
    exit 1
  fi
  sleep 1
done
echo "TorFox from $dmg runs (renderer started)"
