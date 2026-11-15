#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Builds the Windows installer (TorFox-<version>-win-x64-setup.exe).

Stages exactly the files TorFox needs from a release build (an allowlist: a
stray file in the build folder is reported, never shipped), adds the license
texts, and compiles packaging/windows/TorFox.iss with Inno Setup. Inno Setup
is the official release pinned in deps.lock.json; it is checked by
scripts/fetch_deps.py and installed in portable mode under .deps.

Usage (Windows):
  python scripts/package_windows.py [--build-dir out/dev/src/browser/Release]
                                    [--output out/dist] [--version X.Y.Z]
  --test-peruser   installer tests only: a per-user setup under another name
                   and AppId (never published)
  --compression    an Inno Setup compression method (default lzma2/max)
  --next-patch     the version after this one (X.Y.Z+1): the installer tests'
                   update
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import fetch_deps  # noqa: E402

# Every file of a release build that TorFox needs at run time.
REQUIRED_FILES = [
    "TorFox.exe", "TorFox.dll",
    # CEF (cmake/cef_variables.cmake: CEF_BINARY_FILES and CEF_RESOURCE_FILES).
    "libcef.dll", "chrome_elf.dll", "d3dcompiler_47.dll", "dxcompiler.dll", "vk_swiftshader.dll",
    "vk_swiftshader_icd.json", "vulkan-1.dll", "v8_context_snapshot.bin", "icudtl.dat",
    "chrome_100_percent.pak", "chrome_200_percent.pak", "resources.pak",
]
OPTIONAL_FILES = ["dxil.dll"]
# Folders copied whole: Chromium's locale packs and the verified Tor Expert Bundle.
REQUIRED_DIRS = {"locales": re.compile(r"^[A-Za-z0-9_-]+\.pak$"), "tor": None}
REQUIRED_TOR = ["tor/tor.exe", "tor/pluggable_transports/lyrebird.exe",
                "tor/pluggable_transports/pt_config.json", "tor/data/geoip", "tor/data/geoip6"]
# Build by-products that are expected in the build folder and never shipped.
BUILD_ONLY = re.compile(r"\.(pdb|lib|exp|ilk|iobj|ipdb)$", re.IGNORECASE)


class PackageError(Exception):
    pass


def project_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    versions = set(re.findall(r"project\(TorFox VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text))
    if len(versions) != 1:
        raise PackageError(f"could not read one project version from CMakeLists.txt: {versions}")
    return versions.pop()


def inno_compiler() -> Path:
    lock = fetch_deps.load_lock()
    version = lock["innosetup"]["version"]
    home = fetch_deps.DEPS_DIR / "innosetup" / f"InnoSetup-{version}"
    iscc = home / "ISCC.exe"
    if iscc.is_file():
        return iscc
    folder = fetch_deps.fetch("innosetup", "windows", None, False)
    installer = folder / lock["innosetup"]["files"]["windows"]["name"]
    # Portable: no uninstaller, no registry entries, no shortcuts.
    result = subprocess.run([str(installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                             "/CURRENTUSER", f"/DIR={home}", "/NOICONS", "/TASKS=", "/PORTABLE=1"])
    if result.returncode != 0 or not iscc.is_file():
        raise PackageError(f"Inno Setup {version} could not be installed (exit {result.returncode})")
    return iscc


def stage(build: Path, dest: Path) -> list[str]:
    """Copies the release files into |dest|; returns the build files left out."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    missing = [f for f in REQUIRED_FILES if not (build / f).is_file()]
    missing += [f for f in REQUIRED_TOR if not (build / f).is_file()]
    if missing:
        raise PackageError(f"the build in {build} is incomplete; missing: {', '.join(missing)}")
    for name in REQUIRED_FILES + [f for f in OPTIONAL_FILES if (build / f).is_file()]:
        shutil.copy2(build / name, dest / name)
    for folder, pattern in REQUIRED_DIRS.items():
        src = build / folder
        if pattern is None:
            shutil.copytree(src, dest / folder)
            continue
        (dest / folder).mkdir()
        for f in src.iterdir():
            if f.is_file() and pattern.match(f.name):
                shutil.copy2(f, dest / folder / f.name)
    if not any((dest / "locales").glob("*.pak")):
        raise PackageError("no locale packs were found")

    shipped = set(REQUIRED_FILES + OPTIONAL_FILES) | set(REQUIRED_DIRS)
    left_out = [p.name for p in build.iterdir() if p.name not in shipped and not BUILD_ONLY.search(p.name)]

    cef = fetch_deps.fetch("cef", _cef_key(), None, False)
    licenses = dest / "licenses"
    licenses.mkdir()
    shutil.copy2(REPO / "LICENSE", licenses / "TorFox-MPL-2.0.txt")
    shutil.copy2(cef / "LICENSE.txt", licenses / "CEF-LICENSE.txt")
    if (cef / "CREDITS.html").is_file():
        shutil.copy2(cef / "CREDITS.html", licenses / "Chromium-CREDITS.html")
    else:
        raise PackageError(f"{cef} has no CREDITS.html (Chromium's third-party notices)")
    for f in (REPO / "packaging" / "licenses").iterdir():
        shutil.copy2(f, licenses / f.name)
    shutil.copy2(REPO / "packaging" / "THIRD_PARTY_NOTICES.txt", dest / "THIRD_PARTY_NOTICES.txt")
    return left_out


def _cef_key() -> str:
    """The CEF distribution this checkout was configured with (standard or minimal)."""
    root = fetch_deps.DEPS_DIR / "cef"
    for key in ("windows64-standard", "windows64-minimal"):
        name = fetch_deps.load_lock()["cef"]["files"].get(key, {}).get("name")
        if name and (root / f"{name}.verified").exists():
            return key
    raise PackageError("no verified Windows CEF distribution in .deps (run scripts/fetch_deps.py cef ...)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=REPO / "out" / "dev" / "src" / "browser" / "Release")
    ap.add_argument("--output", type=Path, default=REPO / "out" / "dist")
    ap.add_argument("--version")
    ap.add_argument("--test-peruser", action="store_true")
    ap.add_argument("--compression", default="lzma2/max")
    ap.add_argument("--next-patch", action="store_true")
    args = ap.parse_args()
    if sys.platform != "win32":
        print("package_windows.py runs on Windows", file=sys.stderr)
        return 2
    try:
        version = args.version or project_version()
        if args.next_patch:
            major, minor, patch = version.split(".")
            version = f"{major}.{minor}.{int(patch) + 1}"
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            raise PackageError(f"invalid version {version!r}")
        if not re.fullmatch(r"[a-z0-9]+(/[a-z0-9]+)?", args.compression):
            raise PackageError(f"invalid compression {args.compression!r}")
        variant = "peruser-test" if args.test_peruser else "release"
        staged = REPO / "out" / "package" / "windows" / variant / "TorFox"
        left_out = stage(args.build_dir.resolve(), staged)
        for name in left_out:
            print(f"note: {name} is in the build folder and is not packaged", file=sys.stderr)
        iscc = inno_compiler()
        args.output.mkdir(parents=True, exist_ok=True)
        cmd = [str(iscc), "/Q", f"/DAppVersion={version}", f"/DSourceDir={staged}",
               f"/DOutputDir={args.output.resolve()}", f"/DCompression={args.compression}"]
        if args.test_peruser:
            cmd.append("/DTestPerUser")
        cmd.append(str(REPO / "packaging" / "windows" / "TorFox.iss"))
        if subprocess.run(cmd).returncode != 0:
            raise PackageError("Inno Setup could not compile the installer")
        suffix = "-peruser-test" if args.test_peruser else ""
        setup = args.output.resolve() / f"TorFox-{version}-win-x64-setup{suffix}.exe"
        if not setup.is_file():
            raise PackageError(f"{setup} was not written")
        print(setup)
    except (PackageError, fetch_deps.VerificationError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
