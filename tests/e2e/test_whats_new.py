# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""What's new: after an update, TorFox opens torfox://whatsnew/ once.

TorFox keeps the version that last ran in <user data>/last-version. The page
opens only when that record names an older version (an update), never for a
new profile, after a downgrade or for a damaged record. Its notes are the
ones built into TorFox (docs/release-notes/<version>.txt), shown as text."""

from __future__ import annotations

import pathlib
import re
import shutil
import tempfile
import time
import unittest

from harness import REPO, TorFox, bridge_call, shell_state, wait_for, wait_state

VERSION = re.search(r"project\(TorFox VERSION (\d+\.\d+\.\d+)",
                    (REPO / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
NOTES = (REPO / "docs" / "release-notes" / f"{VERSION}.txt").read_bytes().decode("utf-8")
NOTES = NOTES.replace("\r\n", "\n").replace("\r", "\n")

PAGE = """(() => {
  const r = document.querySelector('tf-whats-new')?.shadowRoot;
  const t = r?.querySelector('#title');
  if (!t) return null;
  const n = r.querySelector('#notes');
  return {title: t.textContent, from: r.querySelector('#from').textContent,
          notes: n ? n.textContent : null, markup: n ? n.children.length : -1,
          missing: !!r.querySelector('#notes-missing'), doc: document.title};
})()"""


class WhatsNewTest(unittest.TestCase):
    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-whatsnew-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        self.record = self.user_data / "last-version"

    def start(self) -> TorFox:
        tf = TorFox(user_data=self.user_data).start()
        self.addCleanup(tf.cleanup)
        return tf

    def tab_urls(self, tf: TorFox) -> list[str]:
        state = wait_state(tf.shell(), lambda s: s["tabs"] and not any(t["loading"] for t in s["tabs"]),
                           "tabs loaded")
        return [t["url"] for t in state["tabs"]]

    def test_it_opens_once_after_an_update(self) -> None:
        self.record.write_text("0.0.1\n", encoding="utf-8")
        tf = self.start()
        opened = tf.wait_log("whats_new_opened", timeout=10)
        self.assertEqual(opened["from"], "0.0.1")
        shell = tf.shell()
        state = wait_state(shell, lambda s: [t["url"] for t in s["tabs"]] ==
                           ["torfox://newtab/", "torfox://whatsnew/"], "What's new beside a new tab")
        self.assertEqual(state["nav"]["url"], "torfox://whatsnew/")
        page = tf.page_at("torfox://whatsnew/")
        shown = wait_for(lambda: page.evaluate(PAGE), 10, "the page")
        self.assertEqual(shown["title"], f"TorFox {VERSION}")
        self.assertIn("updated from version 0.0.1", shown["from"])
        # TorFox did not hand this version to a setup itself (no record): the
        # page does not say it was checked.
        self.assertNotIn("signing key", shown["from"])
        # The bundled notes, as text.
        self.assertTrue(NOTES.strip())
        self.assertEqual(shown["notes"], NOTES)
        self.assertEqual(shown["markup"], 0)
        self.assertEqual(shown["doc"], f"What's new in TorFox {VERSION}")
        self.assertEqual(self.record.read_text(encoding="utf-8"), VERSION + "\n")
        self.assertEqual(tf.close(), 0)
        # Not again.
        tf = self.start()
        wait_state(tf.shell(), lambda s: [t["url"] for t in s["tabs"]] == ["torfox://newtab/"], "one tab")
        self.assertEqual(tf.events("whats_new_opened"), [])
        self.assertEqual(tf.events("last_version"), [])  # Already up to date.
        self.assertEqual(tf.close(), 0)

    def test_it_does_not_open_for_a_new_profile_a_downgrade_or_a_damaged_record(self) -> None:
        cases = {"new profile": None, "same version": VERSION, "downgrade": "99.0.0",
                 "garbage": "not a version", "empty": "", "two lines": "0.0.1\n0.0.2\n",
                 "too long": " " * 100 + "0.0.1", "not text": "\x00\xff\xfe"}
        for what, text in cases.items():
            with self.subTest(what):
                if text is None:
                    self.record.unlink(missing_ok=True)
                else:
                    self.record.write_bytes(text.encode("utf-8", "surrogateescape")
                                            if what != "not text" else b"\x00\xff\xfe")
                tf = self.start()
                urls = self.tab_urls(tf)
                self.assertEqual(urls, ["torfox://newtab/"], what)
                self.assertEqual(tf.events("whats_new_opened"), [], what)
                self.assertEqual(tf.close(), 0)
                # From now on the record names this version (one that already
                # did is left as it is).
                self.assertEqual(self.record.read_text(encoding="utf-8").strip(), VERSION, what)
                self.assertEqual(bool(tf.events("last_version")), what != "same version", what)

    def test_an_update_torfox_installed_itself_is_said_to_be_checked(self) -> None:
        # TorFox records the version it hands to the setup just before
        # starting it (update-handoff). This version, found at the next start,
        # is that update.
        self.record.write_text("0.0.1\n", encoding="utf-8")
        handoff = self.user_data / "update-handoff"
        handoff.write_text(VERSION + "\n", encoding="utf-8")
        tf = self.start()
        self.assertEqual(tf.wait_log("update_handoff_record", timeout=10)["outcome"], "installed")
        page = tf.page_at("torfox://whatsnew/")
        shown = wait_for(lambda: page.evaluate(PAGE), 10, "the page")
        self.assertEqual(shown["from"], "TorFox was updated from version 0.0.1. TorFox checked this "
                                        "update against its signing key before installing it.")
        self.assertFalse(handoff.exists())  # Read once.
        self.assertFalse(shell_state(tf.shell())["notice"])
        self.assertEqual(tf.close(), 0)

    def test_an_update_that_did_not_install_is_reported_once(self) -> None:
        # The record names a newer version than this one: the setup failed or
        # never ran. An older one means TorFox was installed again by hand
        # since, and says nothing.
        handoff = self.user_data / "update-handoff"
        for text, outcome in (("99.0.0\n", "not_installed"), ("0.0.1\n", "none"),
                              ("not a version", "none")):
            with self.subTest(text):
                self.record.write_text(VERSION + "\n", encoding="utf-8")
                handoff.write_text(text, encoding="utf-8")
                tf = self.start()
                self.assertEqual(tf.wait_log("update_handoff_record", timeout=10)["outcome"], outcome)
                shell = tf.shell()
                if outcome == "not_installed":
                    state = wait_state(shell, lambda s: s["notice"], "the notice")
                    self.assertEqual(state["notice"],
                                     f"TorFox could not install the update to version 99.0.0. This is "
                                     f"still version {VERSION}. You can try again from Settings > Updates.")
                else:
                    wait_state(shell, lambda s: s["tabs"], "the window")
                    time.sleep(1)
                    self.assertFalse(shell_state(shell)["notice"])
                self.assertEqual(tf.events("whats_new_opened"), [])
                self.assertFalse(handoff.exists())
                self.assertEqual(tf.close(), 0)
        # Once only.
        tf = self.start()
        self.assertEqual(tf.wait_log("update_handoff_record", timeout=10)["outcome"], "none")
        wait_state(tf.shell(), lambda s: s["tabs"], "the window")
        time.sleep(1)
        self.assertFalse(shell_state(tf.shell())["notice"])
        self.assertEqual(tf.close(), 0)

    def test_the_page_opened_by_hand_shows_this_version(self) -> None:
        tf = self.start()
        shell = tf.shell()
        self.assertTrue(bridge_call(shell, "nav.go", {"input": "torfox://whatsnew/"})["ok"])
        page = tf.page_at("torfox://whatsnew/")
        shown = wait_for(lambda: page.evaluate(PAGE), 10, "the page")
        self.assertEqual(shown["title"], f"TorFox {VERSION}")
        self.assertEqual(shown["from"], "The version of TorFox you are using.")
        self.assertEqual(shown["notes"], NOTES)
        tf.close()

    def test_only_the_whats_new_page_can_ask_for_the_notes(self) -> None:
        tf = self.start()
        shell = tf.shell()
        r = bridge_call(shell, "whatsnew.get")
        self.assertFalse(r["ok"], r)
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        settings = tf.page_at("torfox://settings/")
        r = bridge_call(settings, "whatsnew.get")
        self.assertFalse(r["ok"], r)
        self.assertEqual([e["reason"] for e in tf.events("bridge_reject")], ["unknown_command"] * 2)
        # And the page can do nothing else.
        self.assertTrue(bridge_call(shell, "nav.go", {"input": "torfox://whatsnew/"})["ok"])
        page = tf.page_at("torfox://whatsnew/")
        wait_for(lambda: page.evaluate(PAGE), 10, "the page")
        for cmd, args in (("settings.set", {"key": "updates.enabled", "value": False}),
                          ("update.install", None), ("memory.query", None), ("nav.go", {"input": "x"})):
            r = bridge_call(page, cmd, args)
            self.assertFalse(r["ok"], (cmd, r))
        self.assertEqual([e["reason"] for e in tf.events("bridge_reject")], ["unknown_command"] * 6)
        r = bridge_call(page, "whatsnew.get")
        self.assertTrue(r["ok"], r)
        tf.close()


if __name__ == "__main__":
    unittest.main()
