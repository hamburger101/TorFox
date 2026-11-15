# Release notes

One plain-text file per release, named `<version>.txt` (for example `0.2.0.txt`), at most 16 KiB.

The release workflow refuses to run without the file for the version being released. Its text goes into
the signed update manifest, and TorFox shows it in **Settings → Updates** before installing, so it is
covered by the release signature. It is also the description of the GitHub release.

Write what changed for users, especially security fixes and anything that changes behaviour. Plain text
only: TorFox shows it as text, never as HTML or Markdown.

See [`../RELEASING.md`](../RELEASING.md).
