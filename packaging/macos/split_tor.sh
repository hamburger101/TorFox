#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# macOS app bundles may hold only code in Contents/Helpers: code signing
# (codesign --verify --deep --strict) refuses any other file there. Moves
# every file under <helpers> that is neither executable nor a dylib to the
# same place under <resources>, where the app's signature covers it as a
# resource, and removes the folders the move leaves empty.
#
#   sh split_tor.sh <app>/Contents/Helpers/tor <app>/Contents/Resources/tor
set -eu
helpers=$1
resources=$2
[ -d "$helpers" ] || { echo "no such folder: $helpers" >&2; exit 1; }
cd "$helpers"
find . -type f ! -perm -u+x ! -name '*.dylib' | while IFS= read -r f; do
  mkdir -p "$resources/$(dirname "$f")"
  mv -f "$f" "$resources/$f"
done
find . -depth -type d -empty -exec rmdir {} \;
