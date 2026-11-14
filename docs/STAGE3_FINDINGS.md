# Stage 3 findings (2026-11-15)

What Stage 3 (download protection) established, what it found, and what it leaves open. Every claim here
comes from a test in this repository; [`TESTING.md`](TESTING.md) says how to run them. All results were
measured on Windows 11 x64 with CEF 156.0.30 (Chromium 156.0.8078.96), using Release builds. macOS is built
by CI only, and none of the Stage 3 behaviour has been run on macOS.

## Results at a glance
| Suite | Result |
|---|---|
| Unit tests (`torfox_unit_tests`) | 111 passed |
| End-to-end tests (test build) | 145 passed; 2 expected failures, both known gaps listed in `FEATURE_STATUS.md` (the WPAD lookup with Windows proxy auto-detect on; zoom levels kept after clearing history) |
| Mutation check (Stage 3 mutations) | 19 of 19 detected (§5) |

## 1. What was built
- **Two settings** in Settings > Downloads, both now active:
  - *Ask before downloading*, on by default.
  - *Disable all downloads*, off by default. It overrides Ask: with both on, nothing downloads.
- **One decision** (`downloads::Decide`, `src/core/downloads/policy.cc`) applied in three places:
  1. `CanDownload`, before Chromium starts a download. In CEF 156 this refuses every download vector the
     tests can produce, including downloads started without a user action (§3.10);
  2. `OnBeforeDownload`, before any file is named: a second layer, tested on its own with `CanDownload`
     switched off (§4);
  3. A settings observer, and `OnDownloadUpdated`: turning downloads off cancels running downloads at
     once (even stalled ones) and closes any open download prompt, and each later progress update cancels
     them too. The observer is tested on its own with the update check switched off (§4).
- **The prompt** is native: a TaskDialog on Windows and an NSAlert sheet on macOS. It names the file, its
  size and the site it comes from. Program types (`.exe`, `.msi`, `.ps1`, `.app`, `.pkg`, `.dmg`, …) get a
  warning line. *Cancel* is the default button. If the dialog cannot be shown, the answer is no.
- **File names:**
  - TorFox picks every path itself: the last path component only, no control or bidi characters, no
    Windows device names (`CON`, `NUL`, `COM1`, `COM¹`, `CONIN$`, …), at most 200 bytes, keeping a short
    extension.
  - The whole path, with `.crdownload`, is kept within Windows' `MAX_PATH` (259 characters): long names are
    cut, keeping the extension (§3.5).
  - An existing file is never overwritten: the next name is `name (1).ext`, `name (2).ext`, and so on.
    Names are also reserved while a download waits for an answer, so two downloads of the same name
    asked about at the same time get different files.
  - Chromium sanitises names before TorFox sees them. For example, `..\..\x.txt` arrives as `_.._x.txt`,
    and a right-to-left override becomes `_`. It does **not** rename `COM¹.txt` or `CONIN$.txt`, which
    Windows treats as devices; TorFox does (tested end to end, with the name Chromium suggested logged).
- **Partial files:**
  - A download is written as `name.crdownload` and renamed when it is complete. An unfinished file never
    has its final name, and a half-written `setup.exe` can't be mistaken for the real one.
  - CEF otherwise writes straight to the final name (§3.1).
  - TorFox has no resume. Chromium keeps the partial file of a failed download for a resume, and CEF
    cannot cancel a download once it has failed (§3.9). So TorFox checks 2 seconds after any cancel or
    failure, and deletes a partial file that is still there, unless another download has taken that name
    since. (Before reporting a failure, Chromium may restart a download that cannot be resumed; see below.)
  - If Chromium ever resumed a failed download by itself, TorFox would cancel it again. The tests never saw
    Chromium do this, so that safeguard is untested (§6).
  - When Chromium restarts a failed download from the beginning, it keeps the name (and the answer to the
    prompt) it was first given.
- **`torfox://downloads/`** (Ctrl+J; Cmd+Option+L on macOS) lists the downloads. Each entry offers *Cancel*,
  *Show in folder*, *Remove from list* and *Clear finished*.
  - There is deliberately **no "open file"** action.
  - TorFox's list is kept in memory only and is gone when TorFox quits.
  - Chromium's own record of downloads is erased too (§3.8).
  - A downloads button appears in the toolbar once there is something to show, with a badge counting
    running downloads.
- **Private windows** see only their own downloads, and normal windows never see private ones. A private
  window's entries leave the list when it closes. The files themselves stay, like any download.

## 2. Unconfirmed bytes go to a staging folder, not Downloads
Chromium starts receiving a download before TorFox has decided, and keeps receiving it while the prompt is
open. It writes those bytes to a temporary `<guid>.tmp` in its *default download directory*.
- **Where the bytes go:** TorFox sets that directory (`download.default_directory`, read back like every
  other preference) to its own staging folder, `<profile>/Pending Downloads`.
- **What that guarantees:** a download you have not accepted, or have declined, never appears in your
  Downloads folder.
- **Clean-up:** Chromium deletes the temporary file on cancel. TorFox empties the staging folder at every
  start, in case a crash left something behind.

**Pausing does not help.** TorFox first tried to pause a download while its prompt was open. The test showed
the staging file still growing at full speed: Chromium ignores a pause while the download's target is still
being chosen. The pause code was removed rather than kept as a protection that does nothing.

What this means in practice:
- A large file keeps arriving (into the staging folder) while the prompt waits for an answer.
- In a private window, those unconfirmed bytes do touch the disk inside the profile. They are deleted, but
  not securely wiped, if you decline.

Tested by `test_closing_the_tab_cancels_an_open_prompt`, which checks that the download folder stays empty
while the prompt is held, and by `test_ask_and_cancel_saves_nothing`.

## 3. CEF-specific behaviour found by the tests
1. **Partial files use the final name.** Without TorFox's `.crdownload` target, CEF writes the partial
   download straight to its final name (seen with a slow fixture). Fixed by downloading to a partial name
   and renaming on completion. The rename stays within the folder, so the file's "downloaded from the
   internet" mark is kept (next point).
2. **Mark of the Web is applied.** A finished download carries a `Zone.Identifier` stream with `ZoneId=3`
   (Internet), which SmartScreen and Office Protected View rely on (`test_finished_files_are_marked_as_downloaded`).
   - The stream also records the URL the file came from (`HostUrl=`). Every Windows browser does this, and
     it applies to downloads from private windows too.
   - On macOS the equivalent is the `com.apple.quarantine` attribute. It is untested there.
3. **A download ends with its tab.** CEF ties a download to the browser that started it. Closing the tab (or
   its window) cancels the download, and CEF says nothing to the download handler.
   - TorFox records it as cancelled ("tab closed") and shows *"Closing the tab cancelled its download."*
   - This matters for links that open a new tab which only downloads: that tab has to stay open until the
     download finishes.
   - Tested by `test_unfinished_files_never_have_their_final_name`.
4. **`showSaveFilePicker` hangs, then crashes.** In Alloy-style browsers, a File System Access save into a
   folder Chromium considers sensitive (anything under `AppData\Local\Temp`, for example) hangs: the
   restricted-folder warning has no UI to show. Closing the tab afterwards **crashes the browser process**
   (access violation).
   - TorFox now turns off the local File System Access pickers: `showSaveFilePicker`,
     `showOpenFilePicker` and `showDirectoryPicker`, with `--disable-blink-features=FileSystemAccessLocal`,
     forced by the command-line policy.
   - This also closes a way for pages to write files outside the download path. Firefox, Safari and Tor
     Browser don't offer these pickers either.
   - `<input type="file">` uploads are unaffected.
   - Tested by `test_file_system_access_pickers_are_absent`, on a secure origin, where the pickers would
     otherwise exist.
5. **Paths longer than `MAX_PATH` fail, and leave their partial file.** A 200-byte name in a deep folder
   made the download fail (`FILE_FAILED`), and the partial file could then not be deleted. TorFox now cuts
   names so the whole path fits (`DownloadPath.StaysWithinThePathLimit`, and the long-name case of
   `test_names_are_made_safe_and_unique`).
6. **A restart asks for the target again.** When a server cuts a download that cannot be resumed, Chromium
   restarts it from the beginning, up to five times, and calls `OnBeforeDownload` again each time with the
   same download. Before the fix, every restart got a new entry and a new name (`x (1).bin`, …).
   `test_restarted_downloads_keep_their_name`.
7. **Download ids are per profile.** Chromium numbers downloads per profile, and each private window has
   its own off-the-record profile. So a normal and a private download can both be "download 1". TorFox
   gives every entry its own id, and matches Chromium's updates by (profile, Chromium id).
8. **Chromium keeps its own record of downloads.** The normal profile's `History` database records every
   download (the file's path and the URL chain), even though TorFox turns off Chromium's browsing history
   (`history.saving_disabled`, which does stop visits from being saved). `DownloadMetadata` holds the latest
   download's details.
   - CEF has no way to turn this off, so TorFox erases those rows (SQLite `secure_delete`, then `VACUUM`)
     and deletes `DownloadMetadata` whenever Chromium is not running: before CEF starts and after it shuts
     down.
   - While TorFox runs, the record exists on disk. After a crash it stays until the next start.
   - Tested by `ChromiumProfileTest.*` and `test_download_records_are_erased` (quit and crash).
9. **A failed download cannot be cancelled.** For a network failure Chromium could resume, Chromium keeps
   the `.crdownload` file. Calling CEF's `Cancel()` on the interrupted download does nothing: no update
   follows it, and the file stays. (The first version of TorFox called it anyway; the mutation check showed
   that removing the call changed nothing, and logging the updates showed why.) TorFox now deletes the file
   itself 2 seconds later, and Chromium's record of the download is erased at exit (§3.8).
   - `test_failed_downloads_leave_nothing_behind` checks the outcome: no file, and no resume requested.
   - `test_leftover_partial_files_are_removed` checks the clean-up on its own.
   - Chromium did not resume the download by itself in any test run.
10. **`CanDownload` sees every vector.** Contrary to its documentation ("in response to a user-initiated
    action"), CEF 156 calls `CanDownload` for all 11 vectors in §4, including scripted, iframe and
    auto-download ones. So `OnBeforeDownload`'s check never runs in the main matrix, and has its own test.

## 4. The bypass matrix
`test_downloads.py` starts each of 11 download vectors in each mode:
- **Vectors:**
  - `Content-Disposition: attachment` link;
  - a typed URL to an `application/octet-stream`;
  - `<a download>`, a blob URL and a data URL;
  - a scripted click without a gesture;
  - `location.href` on load and `<meta refresh>`;
  - an iframe, `window.open` and a form POST.
- **Disable on (with Ask also on):** no file, no prompt, nothing in the staging folder. All 11 are refused by
  `CanDownload`.
- **Disable on, with `CanDownload` switched off** (test builds only: `--torfox-test-skip-guards=download`,
  `InnerLayerTest`): all 11 are refused by `OnBeforeDownload` alone, with the same result.
- **Turning downloads off while one runs**, with the per-update check switched off
  (`--torfox-test-skip-guards=download-update`, `SettingsLayerTest`): the settings change alone cancels the
  running download and closes the open prompt.
- **Ask on, the user cancels:** a prompt names the file and its origin, and afterwards no file is left, not
  even a partial one.
- **Ask on, the user accepts:** the exact bytes are saved under the expected name.
- **Both off:** saved without a prompt.

Also tested:
- the default for a new profile (asks);
- Ctrl+S in a page saves nothing;
- web pages and other TorFox pages cannot use `downloads.*`;
- two downloads of the same name waiting at the same time get separate files with the right bytes;
- failed, restarted and stalled downloads;
- the real Downloads folder is untouched by the whole run.

The page context menu has no "Save link/image as" items (Stage 2, `test_web_context_menu_has_no_save_or_inspect`).

## 5. Mutation check
Each protection below was removed on its own. The build was rebuilt, and the listed tests had to fail:
- `Decide` ignoring *Disable*;
- `CanDownload` not refusing, or `OnBeforeDownload` not rechecking;
- the prompt skipped, or its answer ignored;
- unconfirmed bytes written to the download folder;
- turning downloads off not cancelling running ones;
- partial files with their final name;
- closing a tab not recording the cancel;
- private downloads visible in every window;
- names not sanitised, not made unique, not reserved while waiting, or not fitted to the path limit;
- a leftover partial file kept;
- a restart given a new name;
- Chromium's download records kept;
- the File System Access pickers left on;
- the program warning missing.

The first mutation run found three of these undetected (the inner `OnBeforeDownload` check, the settings
observer's cancel and TorFox's own name rules), because another layer hid each of them. Each now has a test
of its own, and writing those tests found the bugs in §3.5 to §3.9.

The second run found two more:
- **The settings observer's cancel**, still hidden: Chromium sends progress updates even for a stalled
  download, and each update cancels it too. `SettingsLayerTest` now switches the update check off.
- **Cancelling a failed download**, which turned out to do nothing in CEF (§3.9). The call was removed,
  and so was that mutation.

One mutation did not compile in the second run, and was fixed.

## 6. Not covered, and why
- **Dragging a link or image out of a page** (to the desktop or Explorer) lets the OS create a file without
  the download path. It needs a real OS drag, which the tests can't do. Untested.
- **Printing to PDF** is a user action in the system print dialog, not a website download. Untested.
- **`OnFileDialog` refusing save dialogs while downloads are disabled** stays as defence in depth. With the
  File System Access pickers gone, no web feature that the tests can reach opens a save dialog any more, so
  it has no test.
- **"Show in folder"** is not run in test builds; there it is logged instead. It opens Explorer or Finder, so
  only its checks are tested: complete downloads only, and the file must still exist.
- **Names that differ only in the case of non-ASCII letters** (`É.txt` and `é.txt`) are not treated as the
  same name while both downloads wait for an answer. The file system then decides; nothing is overwritten
  silently, but one of the two downloads may fail.
- **After a crash**, a download that was running leaves its `name.crdownload` in the Downloads folder.
  TorFox cannot tell its own partial files from other browsers' there, so it leaves them.
- **macOS:** the NSAlert sheet, the Downloads folder lookup and the Finder reveal are written, but have
  not been compiled yet: no macOS CI run has happened. None of it is behaviour-tested.
- **A failed download that Chromium resumes by itself** is cancelled again (`download_resume_refused`).
  Chromium never did this in the tests, so the safeguard has no test.
- **A Tor-tab warning about opening downloaded files** waits for Stage 5.
