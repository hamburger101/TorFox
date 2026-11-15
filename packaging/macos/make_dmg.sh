#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Makes TorFox-<version>-mac-<arch>.dmg from a built TorFox.app.
#
#   packaging/macos/make_dmg.sh <TorFox.app> <version> <arm64|x64> <CEF folder> <output folder>
#
# The app gets the license texts and an ad-hoc signature, applied inside out
# (TorFox has no Apple Developer ID, so Gatekeeper cannot name the publisher;
# see docs/INSTALL.md). The hardened runtime is not turned on: with ad-hoc
# signatures its library validation is untested here, and the build has not
# been run on a Mac outside CI. The DMG holds TorFox.app and a link to
# /Applications, and is checked with hdiutil verify.
set -eu

if [ $# -ne 5 ]; then
  echo "usage: $0 <TorFox.app> <version> <arm64|x64> <CEF folder> <output folder>" >&2
  exit 2
fi
app=$1
version=$2
arch=$3
cef=$4
out=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

case "$arch" in
  arm64|x64) ;;
  *) echo "arch must be arm64 or x64, not '$arch'" >&2; exit 2 ;;
esac
if ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "invalid version '$version'" >&2
  exit 2
fi
for f in "$app/Contents/MacOS/TorFox" "$app/Contents/Info.plist" \
         "$app/Contents/Frameworks/Chromium Embedded Framework.framework" \
         "$app/Contents/Helpers/tor/tor" "$cef/LICENSE.txt" "$cef/CREDITS.html"; do
  if [ ! -e "$f" ]; then
    echo "missing: $f" >&2
    exit 1
  fi
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
ditto "$app" "$stage/TorFox.app"
bundle="$stage/TorFox.app"

# License texts, inside the bundle so that the signature covers them.
lic="$bundle/Contents/Resources/licenses"
mkdir -p "$lic"
cp "$repo/LICENSE" "$lic/TorFox-MPL-2.0.txt"
cp "$cef/LICENSE.txt" "$lic/CEF-LICENSE.txt"
cp "$cef/CREDITS.html" "$lic/Chromium-CREDITS.html"
cp "$repo/packaging/licenses/"* "$lic/"
cp "$repo/packaging/THIRD_PARTY_NOTICES.txt" "$bundle/Contents/Resources/THIRD_PARTY_NOTICES.txt"

# Ad-hoc signatures, innermost code first.
sign() { codesign --force --sign - --timestamp=none "$1"; }
find "$bundle/Contents" -type f \( -name '*.dylib' -o -name '*.so' \) | while IFS= read -r f; do sign "$f"; done
find "$bundle/Contents/Helpers" -type f -perm -u+x | while IFS= read -r f; do sign "$f"; done
sign "$bundle/Contents/Frameworks/Chromium Embedded Framework.framework"
for helper in "$bundle/Contents/Frameworks/"*.app; do
  sign "$helper"
done
sign "$bundle"
codesign --verify --deep --strict --verbose=2 "$bundle"

ln -s /Applications "$stage/Applications"
mkdir -p "$out"
dmg="$out/TorFox-$version-mac-$arch.dmg"
rm -f "$dmg"
hdiutil create -volname "TorFox $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg" >/dev/null
hdiutil verify "$dmg" >/dev/null
echo "$dmg"
