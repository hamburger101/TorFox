# Making a release

Releases are built, signed and published by `.github/workflows/release.yml` when a version tag is
pushed. This page is for the maintainer. What users see is described in [`UPDATING.md`](UPDATING.md) and
[`INSTALL.md`](INSTALL.md).

## Once: the release key
TorFox only installs updates signed with the minisign key whose public half is compiled into it. Until
that key exists, builds never look for updates, and the release workflow refuses to run.

1. Get the official minisign 0.12 (`python scripts/fetch_deps.py minisign windows`, or `linux`/`macos`; it is
   checked against its upstream signature), on a computer you trust.
2. Make the key **without a password**. It will exist only as a GitHub Actions secret, and the workflow
   cannot type a password:
   ```
   minisign -G -W -p torfox-release.pub -s torfox-release.key
   ```
3. Put the public key (the second line of `torfox-release.pub`, starting with `RW`) in
   `config/release.json` as `update_public_key`, and commit it. Builds made from then on check updates
   against it.
4. In the repository on GitHub, **Settings → Environments → New environment**, named `release`:
   - **Required reviewers**: yourself (and any other maintainer). Nothing is signed until one of them
     approves the run.
   - **Deployment branches and tags**: *Selected branches and tags*, with one tag rule `v*`.
   - **Environment secrets → Add secret**: `MINISIGN_SECRET_KEY`, with the whole content of
     `torfox-release.key`. Do **not** make it a repository secret: every workflow could read that one.
5. Delete `torfox-release.key` from the computer, or keep it offline. Anyone who has it can sign updates
   that every TorFox installs. Never commit it.
6. **Settings → Rules → Rulesets**:
   - a tag ruleset for `v*` that restricts creation, update and deletion to maintainers;
   - a branch ruleset for `main` that requires a pull request with a review, so a change to
     `.github/workflows/`, `config/release.json` or `scripts/` cannot land unreviewed.

Only the workflow's **sign** job runs in the `release` environment, so it is the only job that can read
the key. It receives the manifest and nothing else (not the builds, not the build tools), and returns
only the signature. The workflow also refuses a tag whose commit is not on `main`, and every action it
uses is pinned by commit (`tests/tools/test_release.py` checks all three). What these rules cannot stop:
someone who can push to `main` without review, or who can approve the `release` environment, can still
get a release signed. Keep that set of people small.

If the key is ever exposed, make a new one, ship a release signed with the **old** key that carries the
**new** public key, then replace the secret. Builds that never install that release keep trusting the old
key.

## Each release
1. Set the version in `CMakeLists.txt` (`project(TorFox VERSION X.Y.Z …)`, both lines). Versions must
   increase: TorFox ignores any release that is not newer than what is installed.
2. Write the release notes in `docs/release-notes/X.Y.Z.txt` (plain text, at most 16 KiB). TorFox shows
   them before installing.
3. Commit, then tag and push:
   ```
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```
4. The workflow:
   - **preflight**: stops unless the tagged commit is on `main`, the tag is `v` + the project version,
     `config/release.json` names the repository and has the public key, and the release notes exist;
   - **windows**: builds the release, runs the unit and release-build tests, builds
     `TorFox-X.Y.Z-win-x64-setup.exe` (`scripts/package_windows.py`);
   - **macos** (arm64 and x64): builds, runs the unit tests, makes `TorFox-X.Y.Z-mac-<arch>.dmg`
     (`packaging/macos/make_dmg.sh`), then mounts it, copies the app out and runs it;
   - **manifest**: writes `manifest.json` (`scripts/release.py manifest`) and the trusted comment
     `torfox-release version:X.Y.Z timestamp:<now>`;
   - **sign** (environment `release`, waits for a reviewer's approval): signs the manifest with the
     official minisign and checks the signature with it; it fails if the environment has no
     `MINISIGN_SECRET_KEY`;
   - **publish** (no secret): checks the signed release again as TorFox will
     (`scripts/release.py check`), then creates the GitHub release as a draft, uploads the three files and
     the manifest and its signature, and publishes it. TorFox ignores drafts, so it never sees a release
     that is not complete.

The repository must be **public** for TorFox to see releases without signing in to GitHub.

## The manifest
```json
{"format":1,"product":"TorFox","version":"X.Y.Z","timestamp":<unix time>,"notes":"…",
 "assets":{"windows-x64":{"name":"TorFox-X.Y.Z-win-x64-setup.exe","size":…,"sha256":"…"},
           "macos-arm64":{"name":"TorFox-X.Y.Z-mac-arm64.dmg","size":…,"sha256":"…"},
           "macos-x64":{"name":"TorFox-X.Y.Z-mac-x64.dmg","size":…,"sha256":"…"}}}
```
`manifest.json.minisig` is a prehashed minisign signature (the default of minisign 0.12). Its trusted
comment must be exactly `torfox-release version:<version> timestamp:<timestamp>`, the same values as in
the manifest. The rules are in `src/core/update/release.cc`, and `scripts/release.py` follows them.

## Not signed by Microsoft or Apple
TorFox has no Authenticode certificate or Apple Developer ID. It is not distributed through app stores.
Windows SmartScreen and macOS Gatekeeper therefore warn on first install (see [`INSTALL.md`](INSTALL.md)).
Updates rest on the minisign signature alone.
