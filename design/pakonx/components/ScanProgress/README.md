# ScanProgress

The segmented Windows XP progress bar, rebuilt in tokens: blocks of `film` while the lamp sweeps, blocks of `go` for transfers and saves.

**Consumer provides:** `value` (0–100), `tone`, mono captions `left` / `right`, an accessible `label`; call `set()` to update.

- Always caption it: what is happening on the left ("Scanning · frame 14 / 24"), a number on the right.
- Use `film` only for physical scanning; everything after the lamp — streaming, writing TIFFs — is `go`.
- If progress is unknown (boot handshake, re-enumeration), show a StatusChip instead of a fake bar.
