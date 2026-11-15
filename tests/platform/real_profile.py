# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Keeps the platform tests away from the user's own TorFox data.

Release builds (and the installed TorFox) use the real per-user data folder,
%LOCALAPPDATA%\\TorFox: its history, settings and cookies. A test module calls
set_aside() in setUpModule and restore() in tearDownModule. set_aside()
renames the user's folder (if there is one) to TorFox.platform-test-aside, so
the tests start from no data at all; restore() removes whatever the tests
left and puts the user's folder back, then checks that every file in it is
byte for byte what it was. A run that was killed half way leaves the folder
aside; the next set_aside() puts it back first.

Nothing is moved while a TorFox is running (the tests skip themselves then).
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import time

import win_process_inspect as wpi

ASIDE_NAME = "TorFox.platform-test-aside"

_hashes: dict[str, str] | None = None


def user_data_dir() -> pathlib.Path:
    return pathlib.Path(os.environ["LOCALAPPDATA"]) / "TorFox"


def aside_dir() -> pathlib.Path:
    return pathlib.Path(os.environ["LOCALAPPDATA"]) / ASIDE_NAME


def torfox_running() -> bool:
    return any((p.get("Name") or "").lower() == "torfox.exe" for p in wpi.all_processes())


def _file_hashes(root: pathlib.Path) -> dict[str, str]:
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def _remove_test_data() -> None:
    for _ in range(100):
        try:
            shutil.rmtree(user_data_dir())
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"could not remove the tests' data in {user_data_dir()}")


def set_aside() -> None:
    global _hashes
    if torfox_running():
        return
    if aside_dir().exists():
        # Left aside by a run that did not finish. What is in the folder now
        # is most likely that run's data, but it is kept, not deleted.
        if user_data_dir().exists():
            kept = user_data_dir().with_name(f"TorFox.platform-test-leftover-{int(time.time())}")
            user_data_dir().rename(kept)
            print(f"note: {user_data_dir()} was set aside by an unfinished test run; "
                  f"it is back, and the folder found in its place is in {kept}")
        aside_dir().rename(user_data_dir())
    if user_data_dir().exists():
        _hashes = _file_hashes(user_data_dir())
        user_data_dir().rename(aside_dir())


def restore() -> None:
    global _hashes
    if not aside_dir().exists():
        return
    _remove_test_data()
    aside_dir().rename(user_data_dir())
    before, _hashes = _hashes, None
    if before is None:
        return
    after = _file_hashes(user_data_dir())
    if after != before:
        changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        raise AssertionError(f"the user's TorFox data changed while it was set aside: {changed[:20]}")
