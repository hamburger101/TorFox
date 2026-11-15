# Release notes

One plain-text file per release, named `<version>.txt` (for example `0.2.0.txt`), at most 16 KiB.

The release workflow refuses to run without the file for the version being released. Its text goes into
the signed update manifest, and TorFox shows it in **Settings → Updates** before installing, so it is
covered by the release signature. It is also the description of the GitHub release.

The same file is built into TorFox itself (`src/browser/CMakeLists.txt`): after an update, the first start
opens a **What's new** tab (`torfox://whatsnew/`) with it. A build whose version has no file here (a
development build) shows that it has no notes.

Write what changed for users, especially security fixes and anything that changes behaviour. Plain text
only: TorFox shows it as text, never as HTML or Markdown.

See [`../RELEASING.md`](../RELEASING.md).
