# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 3 download protection.

Every way a page can start a download is tried in each mode of Settings >
Downloads:
  Disable on (with Ask also on: Disable wins)  -> no file, no prompt
  Ask on, the user cancels                     -> no file, not even a partial one
  Ask on, the user accepts                     -> the exact bytes are saved
  both off                                     -> saved without a prompt
Pages cannot save files any other way: the File System Access pickers are
turned off, and the context menu has no "Save as" items (test_browsing.py).

The prompt is TorFox's native dialog; test builds answer it from a file
(--torfox-test-dialog-choice) and log what it would have shown. Files go to
a per-run folder (--torfox-test-download-dir); the real Downloads folder is
checked to stay untouched."""

from __future__ import annotations

import json
import pathlib
import shutil
import sqlite3
import tempfile
import time
import unittest

from harness import CTRL, SHIFT, Fixture, TorFox, bridge_call, click_element, press, shell_state, \
    wait_for, wait_state

MARK = "tfxdl"  # In every file name these tests create.

# name -> (page body, how the download starts, file name, payload)
# "gesture": the test clicks #go; "load": the page starts it by itself.


def payload(name: str) -> bytes:
    return f"TorFox download test payload: {name}\n".encode() * 40


def attachment_headers(filename: str, content_type: str = "application/octet-stream") -> dict:
    return {"Content-Type": content_type, "Content-Disposition": f'attachment; filename="{filename}"'}


class Vector:
    def __init__(self, name: str, body: str, trigger: str, filename: str):
        self.name = name
        self.body = body
        self.trigger = trigger  # "gesture" | "load" | "typed"
        self.filename = filename


def vectors(origin: str) -> list[Vector]:
    out = []

    def add(name, body, trigger, filename=None):
        out.append(Vector(name, body, trigger, filename or f"{MARK}-{name}.bin"))

    button = "<button id='go' style='display:block;width:200px;height:60px'>go</button>"
    add("attachment", f"<a id='go' href='/f/attachment' style='display:block;padding:30px'>get</a>", "gesture")
    add("typed", "", "typed")  # The address bar, straight to an octet-stream.
    add("a-download", f"<a id='go' href='/f/a-download' download='{MARK}-a-download.bin' "
                      "style='display:block;padding:30px'>get</a>", "gesture")
    add("blob", button + f"""<script>
        document.getElementById('go').onclick = () => {{
          const b = new Blob([{json.dumps(payload('blob').decode())}], {{type: 'application/octet-stream'}});
          const a = document.createElement('a');
          a.href = URL.createObjectURL(b); a.download = '{MARK}-blob.bin';
          document.body.append(a); a.click();
        }};</script>""", "gesture")
    data_url = "data:application/octet-stream;base64," + __import__("base64").b64encode(payload("data")).decode()
    add("data", f"<a id='go' href='{data_url}' download='{MARK}-data.bin' "
                "style='display:block;padding:30px'>get</a>", "gesture")
    add("scripted", f"""<script>
        setTimeout(() => {{
          const a = document.createElement('a');
          a.href = '/f/scripted'; a.download = '{MARK}-scripted.bin';
          document.body.append(a); a.click();
        }}, 50);</script>""", "load")
    add("auto", "<script>location.href = '/f/auto';</script>", "load")
    add("meta-refresh", "<meta http-equiv='refresh' content='0;url=/f/meta-refresh'>", "load")
    add("iframe", "<iframe src='/f/iframe'></iframe>", "load")
    add("window-open", button + "<script>document.getElementById('go').onclick = () => "
                       "window.open('/f/window-open');</script>", "gesture")
    add("form-post", "<form id='f' method='post' action='/f/form-post'>"
                     "<button id='go' style='display:block;width:200px;height:60px'>go</button></form>",
        "gesture")
    return out


def real_downloads_dir() -> pathlib.Path | None:
    import ctypes
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD), ("d3", wintypes.WORD),
                    ("d4", ctypes.c_ubyte * 8)]
    # FOLDERID_Downloads {374DE290-123F-4565-9164-39C4925E467B}
    guid = GUID(0x374DE290, 0x123F, 0x4565, (ctypes.c_ubyte * 8)(0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B))
    out = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(guid), 0, None, ctypes.byref(out)) != 0:
        return None
    path = pathlib.Path(out.value)
    ctypes.windll.ole32.CoTaskMemFree(out)
    return path


class DownloadBase(unittest.TestCase):
    """One TorFox, a fixture site with every vector, and helpers."""
    EXTRA_ARGS: list[str] = []

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.root = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-dl-"))
        cls.user_data = cls.root / "profile"
        cls.dl = cls.root / "downloads"
        cls.dl.mkdir()
        cls.choice = cls.root / "choice.txt"
        cls.staging = cls.user_data / "Pending Downloads"
        cls.origin = f"http://dl-site.test:{cls.fixture.port}"
        cls.vectors = vectors(cls.origin)
        f = cls.fixture
        for v in cls.vectors:
            f.html(f"/v/{v.name}", f"<!doctype html><title>{v.name}</title><body>{v.body}</body>")
            f.raw(f"/f/{v.name}", 200, attachment_headers(v.filename), payload(v.name))
        f.raw("/f/typed", 200, {"Content-Type": "application/octet-stream"}, payload("typed"))
        cls.real = real_downloads_dir()
        cls.real_before = cls.real_listing()
        cls.tf = TorFox(user_data=cls.user_data, host_rules="MAP dl-site.test 127.0.0.1, MAP *.test ~NOTFOUND",
                        extra_args=[f"--torfox-test-download-dir={cls.dl}",
                                    f"--torfox-test-dialog-choice={cls.choice}", *cls.EXTRA_ARGS]).start()
        cls.shell = cls.tf.shell()
        wait_state(cls.shell, lambda s: len(s["tabs"]) >= 1, "the first tab")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)
            shutil.rmtree(cls.root, ignore_errors=True)

    @classmethod
    def real_listing(cls) -> set[str]:
        if not cls.real or not cls.real.is_dir():
            return set()
        return {p.name for p in cls.real.iterdir() if MARK in p.name or p.suffix in (".crdownload", ".tmp")}

    # ----- helpers -----

    def setUp(self) -> None:
        self.fresh_tab()
        self.clear_dirs()
        self.choice.unlink(missing_ok=True)

    def tearDown(self) -> None:
        self.choice.unlink(missing_ok=True)

    def fresh_tab(self) -> None:
        old = [t["id"] for t in shell_state(self.shell)["tabs"]]
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: s["activeId"] not in old and len(s["tabs"]) == len(old) + 1,
                   "fresh tab")
        for tab_id in old:
            bridge_call(self.shell, "tab.close", {"id": tab_id})
        wait_state(self.shell, lambda s: len(s["tabs"]) == 1, "only the fresh tab")
        wait_for(lambda: not [t for t in self.tf.browser.targets()
                              if t["type"] == "page" and not t["url"].startswith("torfox:")],
                 15, "old web tabs to close")

    def clear_dirs(self) -> None:
        # Earlier downloads must be finished before their files are removed.
        for p in self.dl.iterdir():
            p.unlink()

    def files(self, d: pathlib.Path) -> list[str]:
        return sorted(p.name for p in d.iterdir()) if d.is_dir() else []

    def set_mode(self, disable: bool, ask: bool, close_tabs: bool = True) -> None:
        # Opens the settings tab, or shows it if it is already open.
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        page = self.tf.page(lambda t: t["url"].startswith("torfox://settings/"))
        for key, value in (("downloads.disableDownloads", disable), ("downloads.askBeforeDownload", ask)):
            r = bridge_call(page, "settings.set", {"key": key, "value": value})
            self.assertTrue(r["ok"], r)
        if close_tabs:
            self.fresh_tab()
        else:
            bridge_call(self.shell, "tab.new")

    def mark(self) -> int:
        return len(self.tf.log())

    def since(self, mark: int, event: str, **match) -> list[dict]:
        return [e for e in self.tf.log()[mark:] if e.get("ev") == event and
                all(e.get(k) == v for k, v in match.items())]

    def wait_since(self, mark: int, event: str, timeout: float = 15.0, **match) -> dict:
        return wait_for(lambda: (self.since(mark, event, **match) or [None])[-1], timeout,
                        f"{event} {match}")

    def go(self, url: str) -> None:
        self.assertTrue(bridge_call(self.shell, "nav.go", {"input": url})["ok"])

    def start(self, v: Vector) -> None:
        """Opens |v|'s page in the fresh tab and starts its download."""
        if v.trigger == "typed":
            self.go(f"{self.origin}/f/typed")
            return
        url = f"{self.origin}/v/{v.name}"
        self.go(url)
        if v.trigger == "gesture":
            page = self.tf.page(lambda t: t["url"] == url)
            click_element(page, "#go")
            self.page = page

    def expected_name(self, v: Vector) -> str:
        # A typed URL without Content-Disposition is named after its path.
        return "typed" if v.trigger == "typed" else v.filename

    def assert_no_files(self, what: str) -> None:
        # Chromium deletes a cancelled download's partial file itself; allow it
        # a moment, then nothing may be left in either folder.
        wait_for(lambda: not self.files(self.dl) and not self.files(self.staging), 10,
                 f"no files after {what}: {self.files(self.dl)} {self.files(self.staging)}")



class DownloadTest(DownloadBase):
    # ----- the matrix -----

    def test_aa_a_new_profile_asks_first(self) -> None:
        # Runs first (alphabetically), before any test changes the settings.
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        page = self.tf.page(lambda t: t["url"].startswith("torfox://settings/"))
        data = json.loads(bridge_call(page, "settings.get")["response"])
        values = {s["key"]: s["value"] for s in data["settings"]}
        self.assertEqual((values["downloads.askBeforeDownload"], values["downloads.disableDownloads"]),
                         (True, False))
        self.fresh_tab()
        m = self.mark()
        self.choice.write_text("cancel")
        self.start(self.vectors[0])
        self.wait_since(m, "dialog_shown", kind="download")
        self.wait_since(m, "download_cancelled", why="declined")
        self.assert_no_files("the default prompt")

    def test_disabled_blocks_every_vector(self) -> None:
        self.set_mode(disable=True, ask=True)  # Disable overrides Ask.
        for v in self.vectors:
            with self.subTest(v.name):
                self.fresh_tab()
                m = self.mark()
                self.choice.write_text("accept")  # Must never be consumed.
                self.start(v)
                blocked = self.wait_since(m, "download_blocked")
                self.assertIn(blocked["why"], ("downloads disabled",
                                               "downloads disabled (before download)"))
                if v.name == "attachment":
                    # A clicked link is refused before Chromium starts anything.
                    self.assertEqual(blocked["why"], "downloads disabled", "not refused by CanDownload")
                time.sleep(0.5)
                self.assertEqual(self.since(m, "dialog_shown"), [])
                self.assertEqual(self.since(m, "download_started"), [])
                self.assertTrue(self.choice.exists(), "the prompt hook was consumed")
                self.assert_no_files(v.name)

    def test_ask_and_cancel_saves_nothing(self) -> None:
        self.set_mode(disable=False, ask=True)
        for v in self.vectors:
            with self.subTest(v.name):
                self.fresh_tab()
                m = self.mark()
                self.choice.write_text("cancel")
                self.start(v)
                shown = self.wait_since(m, "dialog_shown", kind="download")
                self.assertEqual(shown["title"], "Download this file?")
                self.assertEqual(shown["heading"], self.expected_name(v))
                self.assertIn("From http://dl-site.test", shown["details"])
                self.wait_since(m, "download_cancelled", why="declined")
                self.assertEqual(self.since(m, "download_started"), [])
                self.assert_no_files(v.name)

    def test_ask_and_accept_saves_the_file(self) -> None:
        self.set_mode(disable=False, ask=True)
        for v in self.vectors:
            with self.subTest(v.name):
                self.fresh_tab()
                m = self.mark()
                name = self.expected_name(v)
                self.choice.write_text("accept")
                self.start(v)
                self.wait_since(m, "dialog_shown", kind="download")
                started = self.wait_since(m, "download_started")
                self.assertTrue(started["asked"])
                self.wait_since(m, "download_complete")
                wait_for(lambda: self.files(self.dl) == [name], 10, f"{name} in {self.files(self.dl)}")
                self.assertEqual((self.dl / name).read_bytes(), payload(v.name))
                self.assertEqual(self.files(self.staging), [])
                self.clear_dirs()

    def test_allowed_without_asking(self) -> None:
        self.set_mode(disable=False, ask=False)
        for v in self.vectors:
            with self.subTest(v.name):
                self.fresh_tab()
                m = self.mark()
                self.choice.write_text("cancel")  # Must never be consumed.
                self.start(v)
                started = self.wait_since(m, "download_started")
                self.assertFalse(started["asked"])
                self.wait_since(m, "download_complete")
                name = self.expected_name(v)
                wait_for(lambda: self.files(self.dl) == [name], 10, f"{name} in {self.files(self.dl)}")
                self.assertEqual((self.dl / name).read_bytes(), payload(v.name))
                self.assertEqual(self.since(m, "dialog_shown"), [])
                self.assertTrue(self.choice.exists())
                self.clear_dirs()

    # ----- prompts and running downloads -----

    def slow_route(self, name: str) -> str:
        return self.fixture.slow(f"/slow/{name}", attachment_headers(f"{MARK}-{name}.bin"),
                                 b"S" * 65536, 200, 0.05)

    def test_closing_the_tab_cancels_an_open_prompt(self) -> None:
        self.set_mode(disable=False, ask=True)
        m = self.mark()
        self.choice.write_text("hold")
        self.go(self.slow_route("closed-tab"))
        self.wait_since(m, "dialog_shown", kind="download", choice="hold")
        # While the user decides, Chromium keeps receiving the file (pausing
        # has no effect yet), but only into TorFox's staging folder inside
        # the profile: nothing appears in the download folder.
        wait_for(lambda: self.files(self.staging), 10, "the unconfirmed bytes in staging")
        wait_state(self.shell, lambda s: s["downloads"]["active"] == 1, "active download in the shell")
        time.sleep(1.0)
        self.assertEqual(self.files(self.dl), [])
        tab = shell_state(self.shell)["activeId"]
        bridge_call(self.shell, "tab.new")
        self.assertTrue(bridge_call(self.shell, "tab.close", {"id": tab})["ok"])
        self.wait_since(m, "dialog_cancelled", kind="download")
        self.wait_since(m, "download_cancelled", why="declined")
        self.assert_no_files("closing the tab")
        wait_state(self.shell, lambda s: s["downloads"]["active"] == 0, "no active download")

    def test_disabling_cancels_running_downloads_and_prompts(self) -> None:
        self.check_disabling_cancels_running_downloads_and_prompts()

    def check_disabling_cancels_running_downloads_and_prompts(self) -> None:
        self.set_mode(disable=False, ask=False)
        m = self.mark()
        # A stalled download: after a few chunks the server sends nothing, so
        # Chromium reports no more progress, and only the settings change
        # itself can cancel it.
        self.go(self.fixture.slow("/slow/running", attachment_headers(f"{MARK}-running.bin"),
                                  b"S" * 65536, 200, 0.05, stall_after=3))
        started = self.wait_since(m, "download_started")
        # Wait until bytes are arriving in the real target's partial file.
        wait_for(lambda: f"{MARK}-running.bin.crdownload" in self.files(self.dl), 10,
                 "the partial file")
        # Tabs stay open: the downloads belong to them.
        self.set_mode(disable=False, ask=True, close_tabs=False)
        self.choice.write_text("hold")
        self.go(self.slow_route("prompted"))
        self.wait_since(m, "dialog_shown", kind="download", choice="hold")
        m2 = self.mark()
        self.set_mode(disable=True, ask=True, close_tabs=False)
        cancelled = self.wait_since(m2, "download_cancelled", why="downloads disabled", id=started["id"])
        self.assertTrue(cancelled["known"])
        self.wait_since(m2, "dialog_cancelled", kind="download")
        self.assert_no_files("disabling downloads")
        self.assertEqual(self.since(m2, "download_complete"), [])

    # ----- names -----

    def test_names_are_made_safe_and_unique(self) -> None:
        self.set_mode(disable=False, ask=False)
        f = self.fixture
        f.raw("/f/same", 200, attachment_headers(f"{MARK}-same.txt", "text/plain"), b"one")
        cases = {
            # Server-chosen names cannot leave the folder, hide their type with
            # bidi controls, or use Windows device names. (Chromium replaces
            # separators and bidi controls with "_" before TorFox sees the
            # name; TorFox's own rules are unit-tested in downloads_test.cc.)
            "/f/traversal": ('attachment; filename="..\\\\..\\\\' + MARK + '-up.txt"',
                             f"_.._{MARK}-up.txt"),
            "/f/bidi": (f"attachment; filename*=UTF-8''{MARK}-%E2%80%AEtxt.exe", f"{MARK}-_txt.exe"),
            "/f/device": ('attachment; filename="CON.txt"', "_CON.txt"),
            # Long names are cut, keeping the extension: to 200 bytes, and so
            # that the whole path (with ".crdownload") fits in Windows'
            # MAX_PATH. Chromium's download code fails on longer paths.
            "/f/long": (f'attachment; filename="{MARK}-{"L" * 230}.txt"',
                        (f"{MARK}-{'L' * 230}")[:min(196, 259 - len(str(self.dl)) - 1 - 11 - 4)] + ".txt"),
        }
        # Chromium passes these device names through unchanged: only
        # TorFox's own rules rename them.
        unchanged = {
            "/f/com-superscript": ("attachment; filename*=UTF-8''COM%C2%B9.txt", "COM\u00b9.txt",
                                   "_COM\u00b9.txt"),
            "/f/conin": ('attachment; filename="CONIN$.txt"', "CONIN$.txt", "_CONIN$.txt"),
        }
        for path, (disposition, suggested, expected) in unchanged.items():
            f.raw(path, 200, {"Content-Type": "application/octet-stream",
                              "Content-Disposition": disposition}, b"x")
            with self.subTest(path):
                m = self.mark()
                self.go(self.origin + path)
                named = self.wait_since(m, "download_named")
                self.assertEqual((named["suggested"], named["name"]), (suggested, expected))
                self.wait_since(m, "download_complete")
                wait_for(lambda: expected in self.files(self.dl), 10, f"{expected} in {self.files(self.dl)}")
                self.fresh_tab()
        for path, (disposition, _) in cases.items():
            f.raw(path, 200, {"Content-Type": "application/octet-stream",
                              "Content-Disposition": disposition}, b"x")
        for path, (_, expected) in cases.items():
            with self.subTest(path):
                m = self.mark()
                self.go(self.origin + path)
                done = self.wait_since(m, "download_complete")
                self.assertEqual(done["name"], expected)
                wait_for(lambda: expected in self.files(self.dl), 10, f"{expected} in {self.files(self.dl)}")
                self.fresh_tab()
        # The same name twice: the second gets a number, nothing is overwritten.
        for _ in range(2):
            m = self.mark()
            self.go(self.origin + "/f/same")
            self.wait_since(m, "download_complete")
            self.fresh_tab()
        self.assertIn(f"{MARK}-same.txt", self.files(self.dl))
        self.assertIn(f"{MARK}-same (1).txt", self.files(self.dl))
        self.assertEqual((self.dl / f"{MARK}-same.txt").read_bytes(), b"one")
        # Nothing was written outside the download folder.
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["downloads", "profile"])

    def test_downloads_waiting_at_the_same_time_get_different_names(self) -> None:
        # A name is chosen before its file exists. Two downloads of the same
        # name, both waiting for an answer, must not be given the same one.
        self.set_mode(disable=False, ask=True)
        f = self.fixture
        size = 30 * 65536
        first = f.slow("/slow/twin-a", attachment_headers(f"{MARK}-twin.bin"), b"A" * 65536, 30, 0.05)
        second = f.slow("/slow/twin-b", attachment_headers(f"{MARK}-twin.bin"), b"B" * 65536, 30, 0.05)
        m = self.mark()
        self.choice.write_text("accept-after:1500")
        self.go(first)
        self.wait_since(m, "dialog_shown", kind="download", choice="accept-after:1500")
        self.choice.write_text("accept")
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "a second tab")
        self.go(second)
        named = wait_for(lambda: len(self.since(m, "download_named")) == 2 and self.since(m, "download_named"),
                         15, "both downloads named")
        self.assertEqual([n["name"] for n in named], [f"{MARK}-twin.bin", f"{MARK}-twin (1).bin"])
        wait_for(lambda: len(self.since(m, "download_complete")) == 2, 30, "both downloads complete")
        self.assertEqual(self.since(m, "download_failed"), [])
        self.assertEqual((self.dl / f"{MARK}-twin.bin").read_bytes(), b"A" * size)
        self.assertEqual((self.dl / f"{MARK}-twin (1).bin").read_bytes(), b"B" * size)

    def test_failed_downloads_leave_nothing_behind(self) -> None:
        # A download the server would let Chromium resume, whose connection
        # fails. Chromium keeps the partial file for a resume. TorFox has no
        # resume: it deletes the file shortly after, and no resume is ever
        # requested.
        self.set_mode(disable=False, ask=False)
        m = self.mark()
        self.go(self.fixture.slow("/slow/cut", attachment_headers(f"{MARK}-cut.bin"), b"C" * 65536, 50, 0.02,
                                  cut_after=5, resumable=True))
        failed = self.wait_since(m, "download_failed", timeout=60)
        self.assertEqual(failed["name"], f"{MARK}-cut.bin")
        self.assertEqual(failed["reason"], 20)  # A network failure, which Chromium could resume.
        self.assert_no_files("a failed download")
        time.sleep(3)  # Nothing comes back: no resume, no file.
        self.assertEqual(self.fixture.range_requests, [], "Chromium resumed the download")
        self.assertEqual(self.files(self.dl), [])

    def test_restarted_downloads_keep_their_name(self) -> None:
        # A download that cannot be resumed is restarted from the start by
        # Chromium, which then asks for its target again. It keeps its first
        # name: no second entry, no "(1)" copy.
        self.set_mode(disable=False, ask=False)
        m = self.mark()
        self.go(self.fixture.slow("/slow/restart", attachment_headers(f"{MARK}-restart.bin"), b"R" * 65536, 50,
                                  0.02, cut_after=5))
        failed = self.wait_since(m, "download_failed", timeout=60)
        self.assertGreater(len(self.since(m, "download_restarted", id=failed["id"])), 0, "never restarted")
        self.assertEqual([n["name"] for n in self.since(m, "download_named")], [f"{MARK}-restart.bin"])
        self.assertEqual(failed["name"], f"{MARK}-restart.bin")
        self.assert_no_files("a restarted download")

    def test_leftover_partial_files_are_removed(self) -> None:
        # Chromium deletes a failed download's partial file. If one is still
        # there shortly after (here the test puts it back, as a stand-in for
        # a deletion that failed), TorFox deletes it.
        self.set_mode(disable=False, ask=False)
        m = self.mark()
        self.go(self.fixture.slow("/slow/left", attachment_headers(f"{MARK}-left.bin"), b"C" * 65536, 50, 0.02,
                                  cut_after=5))
        self.wait_since(m, "download_failed", timeout=60)
        partial = self.dl / f"{MARK}-left.bin.crdownload"
        wait_for(lambda: not partial.exists(), 2, "Chromium's own deletion")
        partial.write_bytes(b"left over")
        removed = self.wait_since(m, "download_leftover_removed", timeout=10)
        self.assertTrue(removed["ok"])
        self.assertFalse(partial.exists())
        self.assert_no_files("the clean-up")

    def test_programs_are_flagged_in_the_prompt(self) -> None:
        self.set_mode(disable=False, ask=True)
        self.fixture.raw("/f/setup", 200, attachment_headers(f"{MARK}-setup.exe"), b"MZ")
        self.fixture.raw("/f/notes", 200, attachment_headers(f"{MARK}-notes.txt", "text/plain"), b"hi")
        for path, warned in (("/f/setup", True), ("/f/notes", False)):
            with self.subTest(path):
                m = self.mark()
                self.choice.write_text("cancel")
                self.go(self.origin + path)
                shown = self.wait_since(m, "dialog_shown", kind="download")
                self.assertEqual(bool(shown["warning"]), warned, shown)
                self.wait_since(m, "download_cancelled", why="declined")
                self.fresh_tab()

    def test_file_system_access_pickers_are_absent(self) -> None:
        # Pages cannot write files outside the download path: the File System
        # Access API's local pickers are turned off (--disable-blink-features).
        # They exist only in secure contexts, so look on one (loopback).
        url = self.fixture.html("/secure", "<!doctype html><title>secure</title>")
        self.go(url)
        page = self.tf.page(lambda t: t["url"] == url)
        self.assertTrue(page.evaluate("isSecureContext"))
        self.assertEqual(page.evaluate(
            "[typeof showSaveFilePicker, typeof showOpenFilePicker, typeof showDirectoryPicker]"),
            ["undefined"] * 3)
        # Ordinary uploads still work.
        self.assertEqual(page.evaluate("typeof HTMLInputElement.prototype.showPicker"), "function")

    def test_save_page_shortcut_saves_nothing(self) -> None:
        self.set_mode(disable=False, ask=False)
        url = f"{self.origin}/v/attachment"
        self.go(url)
        page = self.tf.page(lambda t: t["url"] == url)
        m = self.mark()
        press(page, "s", CTRL)
        time.sleep(1.5)
        self.assertEqual(self.since(m, "dialog_shown"), [])
        self.assertEqual(self.since(m, "download_started"), [])
        self.assertEqual(self.files(self.dl), [])

    # ----- the downloads page -----

    def open_downloads(self, shell):
        known = {t["targetId"] for t in self.tf.browser.targets()}
        self.assertTrue(bridge_call(shell, "app.openDownloads")["ok"])
        return self.tf.page(lambda t: t["url"].startswith("torfox://downloads/") and
                            t.get("targetId") not in known)

    def list_entries(self, page) -> dict:
        r = bridge_call(page, "downloads.list")
        self.assertTrue(r["ok"], r)
        return json.loads(r["response"])

    def test_downloads_page_lists_and_manages_downloads(self) -> None:
        self.set_mode(disable=False, ask=False)
        self.fixture.raw("/f/listed", 200, attachment_headers(f"{MARK}-listed.bin"), b"L" * 3000)
        m = self.mark()
        self.go(self.origin + "/f/listed")
        done = self.wait_since(m, "download_complete")
        wait_state(self.shell, lambda s: s["downloads"]["count"] >= 1, "the downloads button")
        self.assertTrue(self.shell.evaluate(
            "!!document.querySelector('tf-shell').shadowRoot.querySelector('#downloads')"))
        page = self.open_downloads(self.shell)
        data = self.list_entries(page)
        entry = next(e for e in data["entries"] if e["id"] == done["id"])
        self.assertEqual((entry["name"], entry["state"], entry["received"], entry["size"]),
                         (f"{MARK}-listed.bin", "complete", 3000, "2.9 KB"))
        self.assertEqual(entry["origin"], self.origin)
        self.assertEqual(data["policy"], "allow")
        # Shown on the page, with "Show in folder" and no way to open the file.
        wait_for(lambda: page.evaluate(
            f"!!document.querySelector('tf-downloads').shadowRoot.querySelector('[data-id=\"{done['id']}\"] .reveal')"),
            10, "the entry on the page")
        buttons = page.evaluate("[...document.querySelector('tf-downloads').shadowRoot"
                                ".querySelectorAll('button')].map(b => b.textContent.trim().toLowerCase())")
        self.assertFalse([b for b in buttons if b.startswith("open")], buttons)
        self.assertTrue(bridge_call(page, "downloads.reveal", {"id": done["id"]})["ok"])
        self.wait_since(m, "download_revealed", id=done["id"])
        # Running downloads cannot be removed; finished ones can.
        self.assertFalse(bridge_call(page, "downloads.cancel", {"id": done["id"]})["ok"])
        self.assertTrue(bridge_call(page, "downloads.remove", {"id": done["id"]})["ok"])
        self.assertNotIn(done["id"], [e["id"] for e in self.list_entries(page)["entries"]])
        # Removing it from the list leaves the file alone.
        self.assertIn(f"{MARK}-listed.bin", self.files(self.dl))
        # A deleted file cannot be revealed. (A new tab: nav.go would replace
        # the downloads page.)
        m = self.mark()
        bridge_call(self.shell, "tab.new")
        self.go(self.origin + "/f/listed")
        again = self.wait_since(m, "download_complete")
        for p in self.dl.iterdir():
            p.unlink()
        r = bridge_call(page, "downloads.reveal", {"id": again["id"]})
        self.assertFalse(r["ok"])
        self.assertTrue(bridge_call(page, "downloads.clear")["ok"])
        self.assertEqual(self.list_entries(page)["entries"], [])
        self.assertEqual(self.since(m, "download_revealed"), [])

    def test_downloads_page_shows_when_downloads_are_disabled(self) -> None:
        self.set_mode(disable=True, ask=True)
        page = self.open_downloads(self.shell)
        self.assertEqual(self.list_entries(page)["policy"], "disabled")
        wait_for(lambda: page.evaluate(
            "!!document.querySelector('tf-downloads').shadowRoot.querySelector('#disabled-banner')"),
            10, "the disabled banner")

    def test_private_downloads_stay_in_their_window(self) -> None:
        self.set_mode(disable=False, ask=False)
        self.fixture.raw("/f/private", 200, attachment_headers(f"{MARK}-private.bin"), b"P")
        self.fixture.raw("/f/normal", 200, attachment_headers(f"{MARK}-normal.bin"), b"N")
        m = self.mark()
        self.go(self.origin + "/f/normal")
        normal_done = self.wait_since(m, "download_complete")
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created", private=True))
        self.assertTrue(bridge_call(self.shell, "window.newPrivate")["ok"])
        wait_for(lambda: len(self.tf.events("window_created", private=True)) > created, 15, "private window")
        private_shell = self.tf.new_shell(known)
        wait_state(private_shell, lambda s: s["private"], "private shell")
        self.assertTrue(bridge_call(private_shell, "nav.go", {"input": self.origin + "/f/private"})["ok"])
        private_done = self.wait_since(m, "download_complete", name=f"{MARK}-private.bin")
        self.addCleanup(self.close_private, private_shell)
        # Each window's page lists only what it may see.
        private_page = self.open_downloads(private_shell)
        private_ids = [e["id"] for e in self.list_entries(private_page)["entries"]]
        self.assertEqual(private_ids, [private_done["id"]])
        self.assertTrue(self.list_entries(private_page)["private"])
        normal_page = self.open_downloads(self.shell)
        normal_ids = [e["id"] for e in self.list_entries(normal_page)["entries"]]
        self.assertIn(normal_done["id"], normal_ids)
        self.assertNotIn(private_done["id"], normal_ids)
        # A normal window cannot act on a private download.
        self.assertFalse(bridge_call(normal_page, "downloads.remove", {"id": private_done["id"]})["ok"])
        self.assertFalse(bridge_call(normal_page, "downloads.reveal", {"id": private_done["id"]})["ok"])
        # The file itself was saved like any other.
        self.assertIn(f"{MARK}-private.bin", self.files(self.dl))
        # Closing the private window forgets its downloads.
        self.close_private(private_shell)
        self.fresh_tab()
        normal_page = self.open_downloads(self.shell)
        self.assertNotIn(private_done["id"], [e["id"] for e in self.list_entries(normal_page)["entries"]])

    def close_private(self, shell) -> None:
        # Its shell goes with the window, so no reply comes back: send the
        # raw key only. The test's cleanup calls it again, so it remembers
        # which windows this test closed (another test's window does not count).
        closed = self.__dict__.setdefault("_closed_private", set())
        if id(shell) in closed:
            return
        before = len(self.tf.events("window_destroyed", private=True))
        shell.send("Input.dispatchKeyEvent", {"type": "rawKeyDown", "windowsVirtualKeyCode": 0x57,
                                              "nativeVirtualKeyCode": 0x57,
                                              "modifiers": CTRL | SHIFT})  # Ctrl+Shift+W.
        wait_for(lambda: len(self.tf.events("window_destroyed", private=True)) > before, 15,
                 "private window closed")
        closed.add(id(shell))

    def test_finished_files_are_marked_as_downloaded(self) -> None:
        # Windows' mark of the web (SmartScreen, Office Protected View) is kept
        # through TorFox's rename from the partial name.
        self.set_mode(disable=False, ask=False)
        self.fixture.raw("/f/marked", 200, attachment_headers(f"{MARK}-marked.bin"), b"M" * 100)
        m = self.mark()
        self.go(self.origin + "/f/marked")
        self.wait_since(m, "download_complete")
        path = self.dl / f"{MARK}-marked.bin"
        wait_for(path.exists, 10, "the file")
        zone = pathlib.Path(str(path) + ":Zone.Identifier").read_text(encoding="utf-8")
        self.assertIn("ZoneId=3", zone)
        self.assertEqual(self.files(self.dl), [f"{MARK}-marked.bin"])

    def test_private_downloads_keep_the_mark_but_not_the_address(self) -> None:
        # The mark (ZoneId=3) must stay, but a private window's file must not
        # record where it came from (Chromium can write HostUrl and
        # ReferrerUrl into the mark).
        self.set_mode(disable=False, ask=False)
        self.fixture.raw("/f/private-marked", 200, attachment_headers(f"{MARK}-private-marked.bin"), b"Q" * 50)
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created", private=True))
        self.assertTrue(bridge_call(self.shell, "window.newPrivate")["ok"])
        wait_for(lambda: len(self.tf.events("window_created", private=True)) > created, 15, "private window")
        private_shell = self.tf.new_shell(known)
        self.addCleanup(self.close_private, private_shell)
        wait_state(private_shell, lambda s: s["private"], "private shell")
        m = self.mark()
        self.assertTrue(bridge_call(private_shell, "nav.go", {"input": self.origin + "/f/private-marked"})["ok"])
        self.wait_since(m, "download_complete", name=f"{MARK}-private-marked.bin")
        path = self.dl / f"{MARK}-private-marked.bin"
        wait_for(path.exists, 10, "the file")
        zone = pathlib.Path(str(path) + ":Zone.Identifier").read_text(encoding="utf-8")
        print(f"\n[private Zone.Identifier] {zone!r}", end=" ")
        self.assertIn("ZoneId=3", zone)
        host = self.origin.split("//", 1)[1].split(":", 1)[0]
        self.assertNotIn(host, zone)
        self.assertNotIn("private-marked", zone)
        self.assertNotIn("HostUrl=http", zone)
        self.assertNotIn("ReferrerUrl=http", zone)

    def test_unfinished_files_never_have_their_final_name(self) -> None:
        self.set_mode(disable=False, ask=False)
        m = self.mark()
        self.go(self.slow_route("partial"))
        self.wait_since(m, "download_started")
        wait_for(lambda: f"{MARK}-partial.bin.crdownload" in self.files(self.dl), 10, "the partial file")
        self.assertNotIn(f"{MARK}-partial.bin", self.files(self.dl))
        # Closing its tab cancels it (CEF ties a download to its browser):
        # TorFox records that, tells the user, and the partial file is deleted.
        tab = shell_state(self.shell)["activeId"]
        bridge_call(self.shell, "tab.new")
        bridge_call(self.shell, "tab.close", {"id": tab})
        self.wait_since(m, "download_cancelled", why="tab closed")
        wait_state(self.shell, lambda s: "cancelled its download" in (s["notice"] or ""), "the notice")
        wait_state(self.shell, lambda s: s["downloads"]["active"] == 0, "no active download")
        self.assert_no_files("closing the downloading tab")

    def test_web_pages_cannot_use_download_commands(self) -> None:
        url = f"{self.origin}/v/attachment"
        self.go(url)
        page = self.tf.page(lambda t: t["url"] == url)
        self.assertEqual(page.evaluate("typeof window.torfoxQuery"), "undefined")
        # Other TorFox pages cannot either.
        known = {t["targetId"] for t in self.tf.browser.targets()}
        bridge_call(self.shell, "app.openHistory")
        history = self.tf.page(lambda t: t["url"].startswith("torfox://history/") and
                               t.get("targetId") not in known)
        for cmd in ("downloads.list", "downloads.clear"):
            r = bridge_call(history, cmd)
            self.assertEqual((r["ok"], r.get("code")), (False, 403), cmd)

    # ----- runs last (alphabetically) -----

    def test_zz_real_downloads_folder_untouched(self) -> None:
        if not self.real:
            self.skipTest("no Downloads folder")
        self.assertEqual(self.real_listing() - self.real_before, set())


class InnerLayerTest(DownloadBase):
    """CanDownload refuses every vector above before OnBeforeDownload is
    reached, so OnBeforeDownload's own check never runs in DownloadTest. Here
    CanDownload is switched off (test builds only), and OnBeforeDownload
    alone must stop every vector."""
    EXTRA_ARGS = ["--torfox-test-skip-guards=download"]

    def test_before_download_refuses_every_vector(self) -> None:
        self.assertEqual(self.tf.wait_log("fault_injection")["skip_guards"], ["download"])
        self.set_mode(disable=True, ask=True)
        for v in self.vectors:
            with self.subTest(v.name):
                self.fresh_tab()
                m = self.mark()
                self.choice.write_text("accept")  # Must never be consumed.
                self.start(v)
                blocked = self.wait_since(m, "download_blocked")
                self.assertEqual(blocked["why"], "downloads disabled (before download)")
                time.sleep(0.5)
                self.assertEqual(self.since(m, "dialog_shown"), [])
                self.assertEqual(self.since(m, "download_started"), [])
                self.assertTrue(self.choice.exists(), "the prompt hook was consumed")
                self.assert_no_files(v.name)




class SettingsLayerTest(DownloadBase):
    """Turning downloads off cancels running downloads at once (a settings
    observer), and each later progress update cancels them too. The second
    layer hides the first in DownloadTest, so here it is switched off (test
    builds only)."""
    EXTRA_ARGS = ["--torfox-test-skip-guards=download-update"]

    def test_disabling_cancels_running_downloads_and_prompts(self) -> None:
        self.assertEqual(self.tf.wait_log("fault_injection")["skip_guards"], ["download-update"])
        self.check_disabling_cancels_running_downloads_and_prompts()

    slow_route = DownloadTest.slow_route
    check_disabling_cancels_running_downloads_and_prompts = \
        DownloadTest.check_disabling_cancels_running_downloads_and_prompts


class DownloadRecordsTest(DownloadBase):
    """Chromium records every download of the normal profile in its History
    database (the file's path and the URLs it came from), whatever its
    history settings. TorFox erases those records whenever Chromium is not
    running: after a quit, and at the next start after a crash."""

    def normal_profile(self) -> pathlib.Path:
        return self.user_data / "User Data" / "Normal"

    def records(self) -> int:
        con = sqlite3.connect(f"file:{self.normal_profile() / 'History'}?mode=ro", uri=True)
        try:
            return sum(con.execute(f"SELECT count(*) FROM {t}").fetchone()[0]
                       for t in ("downloads", "downloads_url_chains"))
        finally:
            con.close()

    def restart(self) -> None:
        self.tf.start()
        type(self).shell = self.tf.shell()
        wait_state(self.shell, lambda s: len(s["tabs"]) >= 1, "the first tab")

    def download(self, name: str) -> None:
        self.set_mode(disable=False, ask=False)
        self.fixture.raw(f"/f/{name}", 200, attachment_headers(f"{MARK}-{name}.bin"), b"R")
        m = self.mark()
        self.go(self.origin + f"/f/{name}")
        self.wait_since(m, "download_complete")

    def test_download_records_are_erased(self) -> None:
        self.assertTrue(self.tf.wait_log("download_records_erased")["ok"])
        # A normal quit: erased after CEF has shut down.
        self.download("record-quit")
        self.tf.close()
        self.assertEqual(self.records(), 0)
        self.assertFalse((self.normal_profile() / "DownloadMetadata").exists())
        # A crash: Chromium's record stays on disk until the next start.
        self.restart()
        self.download("record-crash")
        time.sleep(12)  # Chromium commits its History database every 10 s.
        self.tf.kill()
        self.assertGreater(self.records(), 0, "Chromium did not record the download (test assumption)")
        self.restart()
        erased = self.tf.wait_log("download_records_erased")
        self.assertTrue(erased["ok"])
        self.assertGreater(erased["rows"], 0)
        self.tf.close()
        self.assertEqual(self.records(), 0)


if __name__ == "__main__":
    unittest.main()
