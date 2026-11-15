# Updater test vectors

Made with the official minisign 0.12 (`deps.lock.json`, section `minisign`) and two key pairs created for
these tests only. **The secret keys were deleted once the signatures were made**; only the public keys and
signatures are here.

| File | What it is |
|---|---|
| `a.pub`, `b.pub` | Public keys A and B (`minisign -G -W`) |
| `manifest.json` | A release manifest for TorFox 1.2.3 (no trailing newline) |
| `good.minisig` | `minisign -S` with key A, trusted comment `torfox-release version:1.2.3 timestamp:1790000000` |
| `wrong_key.minisig` | The same, with key B |
| `legacy.minisig` | `minisign -S -l` (legacy, not prehashed) with key A: TorFox must refuse it |
| `comment_mismatch.minisig` | Key A, trusted comment naming version 9.9.9: valid signature, wrong release |
