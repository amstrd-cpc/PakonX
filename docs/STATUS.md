# Status

Last updated: 2026-10-23. Device and protocol facts live in
[OEM_RE.md](OEM_RE.md) (single reference); this page only says what works,
how it is verified, and what is open. Hardware: F-135+ serial 17373 on
Windows 11 x64 (WinUSB); the OEM stack runs in an XP VM for comparison.

## Scanning (`pakon-cli scan`) — verified on hardware

| Area | Verification |
|---|---|
| Cold→warm boot chain (FX2 RAM boot only) | hardware 2026-10-07, [BOOT_CHAIN.md](BOOT_CHAIN.md) |
| `list`, `descriptors`, `probe`, `identify`, `status` | hardware 2026-10-08/10; identify sends the OEM pre-init (`0x97`, `0x03`) |
| Read-only EEPROM reader (`pakon-cli eeprom`): allow-list, CRC, primary/backup, marked 16402 fallback, colour matrices | hardware 2026-10-10 and 2026-10-20: serial 17373, Offset 35/70/69, speeds, adjust; NegMatrix = the OEM registry copy to all digits |
| First light Base 4 / Base 4-IR | hardware 2026-10-10 |
| Corrections at the OEM's start on-times (OEM_RE §9) | Base 4 roll1/river1: LED R3 G14 B7 = the OEM's on this unit; Base 16-IR film16ir1: R5 G24 B12 IR4 = the OEM's; Base 8 strip8: R4 G20 B10; Base 16 strip16: R3 G12 B7 |
| Dark offset servo | settles in 2–3 rounds since `discard()` also drops the reads queued before it (stale reads of the previous state had made it oscillate) |
| Film pass, all bases, streamed to `.pakraw` | Base 4: river1 (2026-10-14), roll1 (full roll, 75 000-row cap); Base 8: strip8, strip8dx; Base 16: film16ir1 (IR, 2026-10-13), strip16 (2026-10-22). 0 overflows, 0 resyncs, teardown acknowledged on every run; line periods 0.904 / 1.355 / 1.969 ms (IR 2.407) |
| Row cap mid-film | roll1: the motor runs the film out until the DX sensor is clear, then the teardown |
| DX records | strip8dx: 25 records kept in the sidecar; film code 39/1 and frame numbers decoded offline (below) |

Simulator coverage on top: all six modes, first light, late film, no film,
slow consumer, corrupt EEPROM, a fault at every frame, exception mid-film,
run-out limits, DX record capture.

## Processing (`pakon-cli frames`) — offline, verified against the OEM

| Area | Verification |
|---|---|
| Framing = the OEM's (OEM_RE §13.2), Base 4/8/16 | frame starts vs the OEM's, registered by the Ansel thumbnails: river1 4–8 lines; strip8/strip16 6–15 lines except one marginal picture per strip |
| Blank / fogged labelling | roll1: 36 pictures, leader and tail labelled |
| Stage-1 positive (`--positive`) | own neutral conversion (`process/positive.hpp`); not the OEM look |
| TLB density (OEM_RE §13.3) | river1 vs the OEM's own PakonIMAu input: bias ≤ 2 (density × 1000), no free parameter; Base 8 bias ≤ 2.3 |
| The OEM Pakon look (`--pakon-look`, OEM_RE §13.4) | the OEM's PakonIMAu.dll via `tools/oem_host`; Base 8 replays byte-identical; river1 and strip8 JPEGs match the OEM's by eye, JPEG structure identical; Base 4/8/16 geometry |
| DX film code and frame numbers (OEM_RE §10.1) | strip8dx: product 39 specifier 1 = the OEM's DxCode.txt; pictures 19A..24A = the OEM's file names |

## Build and tests

MSVC (VS 18, `/W4 /permissive-`): zero warnings on a clean build, 14 CTest
entries, 131 C++ cases; the Python tool tests (9) run under WSL. The Linux
builds (GCC, clang ASan/UBSan/TSan, MinGW-w64 under Wine) were last run on
2026-10-10 and have not been re-run since the processing library was added.

## Riskiest assumptions (ranked)

1. **Teardown tail `rate=0 → go → 0xA2`** (kept from the bridge per the
   safety rules). The OEM never writes a rate below 1000; it has been
   acknowledged on every hardware run so far.
2. **Film on-time boost R1.39 G2.51 B5.18** comes from capture ratios (this
   unit's OEM run: 1.397 2.514 5.05), not from TLB code.
3. **DX sensor-to-gate shift 0.35 pitch** is measured on one strip; frame
   numbers on another roll may be off by one if it differs.
4. **Base 16 R/B line shift 8** comes from our own film16ir1, not from an
   OEM Base 16 dump (the Base 8 shift 6 was checked on one).
5. **dmin for Ansel** is our estimate (1st percentile of the thumbnails),
   2–4 % above TLB's on strip8; it shifts the Ansel balance slightly.
6. **Film start/end detector** is this stack's own (TLB's is unknown); the
   row cap and the run-out bound it.
7. **LED clock 0.24 per integration unit** (fraction dropped: matches the
   OEM's base word in all six captured configurations since 2026-11-17;
   until then the IR modes sent 511 / 600 instead of 510 / 599), DX words and
   lamp/TEC values are fitted or replayed from captures, not derived.

## Open

- Digital ICE: the OEM's `DMLDICELib.dll` (OEM_RE §13.5) is still not
  driven, and the IR-only reference step (lamp mask `0x02`) is not
  implemented. PakonX has its own infrared dust removal instead
  (`process/ice.hpp`, `frames --ice`, 2026-11-06): it removes dust and hair
  on 7908 (Base 4 IR) and 7919 (Base 16 IR) and leaves silver B&W 13212
  as scanned.
- Built, not yet run on the scanner (2026-11-06): `scan --film` (exposure
  per film kind), `--film-gate`, `--hot-start` (batch scanning). Simulator
  tests only. Half-frame placement (`frames --format`) is tested on
  synthetic profiles only: no half-frame scan yet.
- Slides (ColRev). Colour negative (CN-Enhanced) and black-and-white negative
  are covered: B&W runs TLB's BnW_NORMAL path (roll +0x20 = 4, scene balance
  +0x48 = 4, colour adjust +0x4c = 1), a replay byte-identical to the OEM's
  B&W JPEG (VM proxy bwref, 2026-10-23); PakonX's own look turns grey.
- PakonX (`apps/pakonx`): light table, looks and saving work on scans on
  disk; the scan window (read-only checks, film pass, Stop through
  `--stop-on-stdin`) is built and tested off the device, not yet run on
  the scanner. On start it boots a cold scanner with the frozen
  PowerShell reference (embedded; firmware from the app, no network) and
  identifies it; Scan runs the read-only checks itself; not yet run on the
  scanner either.
- C++ FX2 loader ([LOADER_DESIGN.md](LOADER_DESIGN.md)); the PowerShell
  reference procedure remains the boot path.
- OEM_RE §12 open questions.
