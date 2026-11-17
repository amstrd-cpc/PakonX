# Pakon — native stack for the Kodak/Pakon F-135 / F-135+ film scanner

A from-scratch **C++20 / CMake** implementation of everything between a
USB port and a scan session: device enumeration, WinUSB transport, the
PPB protocol layer, scanner session logic, and a command-line tool —
plus the reverse-engineered **cold→warm firmware boot chain**, validated
end-to-end on real hardware (2026-10-07).

Three rules shape everything here:

- **Nothing is invented.** Every protocol claim is cited to the primary
  spec (`pakon-reference`) or the `alibosworth/pakon-captures` corpus and
  carries an evidence marker — `[DOCUMENTED]`, `[CONFIRMED]`, `[STATIC]`,
  `[PROVEN]`, `[INFERRED]`, `[UNKNOWN]` — so you can always see how far
  the evidence reaches.
- **Hardware is treated carefully.** All writes stay volatile, safety
  prohibitions are test-pinned, and hardware runs happen as single
  explicitly-approved commands whose output is recorded verbatim.
- **Firmware bytes never enter this repository** (scope rule of the
  primary spec). The boot procedure fetches the OEM artifacts at run
  time and MD5-gates them host-side before any USB operation.

## Where things stand

| Area | State |
|---|---|
| Build & tests | **14 CTest entries / 131 C++ + 9 Python cases**, zero warnings on MSVC (2026-10-22); GCC, clang ASan+UBSan/TSan and MinGW-w64 last run 2026-10-10 |
| USB layer | Two-pass SetupAPI enumeration (finds even Code-28 units), WinUSB transport, one shared overlapped-open path — **cold and warm bindings both observed on hardware** |
| Bootstrap probe | `pakon-cli probe` — exactly one read-only `0xA9` read — **hardware-validated**: cold ROM baseline `win32 121`, answers `C0-05-0F-35-F2-07-AA-04` once stage-1 runs |
| Descriptor discovery | `pakon-cli descriptors` — raw device + configuration descriptors, full interface/endpoint topology, strings, WinUSB cross-check; standard `GET_DESCRIPTOR` only — **recorded on hardware 2026-10-08** (live device descriptor byte-identical to the Pakon7 image) in [docs/F135_TOPOLOGY.md](F135_TOPOLOGY.md) |
| Cold→warm boot chain | **Validated on hardware 2026-10-07**: stage-1 upload → `0xA4` preamble → gated `0xA9` → Pakon7 download (709 × `0xA3` + 19 × `0xA0`) → final run → re-enumeration as `0F05:F135` — full evidence record in [docs/BOOT_CHAIN.md](BOOT_CHAIN.md) |
| C++ loader | **Written for Linux/macOS (`pakon-cli boot`), not yet run on hardware** — [docs/LOADER_DESIGN.md](LOADER_DESIGN.md); on Windows the frozen PowerShell procedure ([tools/pakon_boot_reference.ps1](../tools/pakon_boot_reference.ps1)) remains the trusted reference until each ported step is re-validated |
| PPB + scanner session | `identify`/`status` on hardware 2026-10-08/09; identify now sends the OEM pre-init (`0x97`, `0x03` page select) |
| Scan path | EEPROM reader (read-only), concurrent EP6 stream, OEM-order runner with Corrections, film window streamed to `.pakraw`, run-out, DX records, teardown on every path; `pakon-cli scan` writes `.pakraw` + TIFF preview + stats + `.scan.json` — **hardware: film passes in Base 4, 8 and 16 (incl. IR)**, calibration equal to the OEM's on this unit ([docs/HARDWARE_RUNBOOK.md](HARDWARE_RUNBOOK.md)) |
| Processing (`pakon_process`, `pakon-cli frames`) | Frames placed like the OEM's framing; stage-1 positives; **the OEM "Pakon look"** through the OEM's own PakonIMAu.dll (32-bit helper `tools/oem_host`, Base 4/8/16, byte-identical replay); film DX code and frame numbers — verified against the OEM's own data ([docs/OEM_RE.md](OEM_RE.md) §10.1, §13). Digital ICE not started |
| Driver package | [driver/PakonWinUSB.inf](../driver/PakonWinUSB.inf) — in-box `winusb.sys`, both device identities, interface GUID `{0e9e6f29-…}`; [driver/README.md](../driver/README.md) |

Latest full status, including open questions and what is still
untested: **[docs/STATUS.md](STATUS.md)**.

## Quick start

### Build & test (Linux / any host)

```sh
cmake -S . -B build -G Ninja        # CMake ≥ 3.20, C++20; any generator
cmake --build build
ctest --test-dir build --output-on-failure   # expect: 14/14 entries
```

### Build & test (Windows / MSVC)

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # expect: 14/14
```

### The CLI

```
pakon-cli [--log trace] <command>

  list       enumerate attached Pakon scanners (no I/O sent)
  attrib     PnP/driver attribution report (read-only property queries)
  descriptors  read-only USB topology of the booted F135: device +
             configuration descriptor bytes, every interface/endpoint,
             strings (standard GET_DESCRIPTOR reads + WinUSB queries)
  probe      read-only bootstrap probe: one documented 0xA9 control read
  identify   open a PPB session and detect the scanner model
  status     identify + read-only status polls and register reads
  eeprom     read-only EEPROM dump + this unit's per-mode values
  scan       --dry-run | --live-scan (see `pakon-cli --help` and
             docs/HARDWARE_RUNBOOK.md)
  frames     offline: split a scan into its pictures (OEM framing), DX
             film code and frame numbers; raw TIFFs, --positive,
             --save jpeg|tiff [--pictures 1,4-6] [--dx-names] (finished
             pictures), or
             --pakon-look (the OEM look via tools/oem_host, Windows)
```

Binaries: `build/apps/pakon-cli/pakon-cli` (single-config) or
`build\apps\pakon-cli\Release\pakon-cli.exe` (MSVC). `--log trace`
prints full TX/RX packet hex dumps.

## Hardware safety charter (enforced by code and tests)

- EEPROM/I2C vendor paths, bootloader addresses and type-byte-0 PPB
  frames have no call site in the bootstrap path — pinned as forbidden
  by `bootstrap_probe_test` and `usb_identity_test`.
- The boot sequence writes FX2 RAM and the halt/run registers only, with
  addresses gated below `0xC000` — **unplug/replug always restores the
  cold device**.
- Gates abort, never retry; exactly one gated `0xA9` per boot; both
  firmware artifacts are MD5-verified host-side before any USB traffic.
- `tools/pakon_boot_reference.ps1` is a frozen, proven procedure — run
  it only as an explicitly approved experiment.
- The EEPROM is read only: no `0xA2` at any wIndex, no write-select on
  `0xA4` (`eeprom::is_allowed` plus the WinUSB backend guard). The PIC
  bootloader addresses `0x22/0x26/0x42/0x46` and `0xA2/0xA4` on the PPB
  bus are never addressed. PIC firmware is never flashed; the OEM
  firmware-update paths are documented, not implemented.
- LED currents (PICL `0x81`) are clamped to `LED_CEILINGS` for the probed
  board and IR state (strictest row when unknown); Corrections output is
  clamped too. TEC writes replay OEM-recovered values only.
- Every path that lights the lamp or moves the motor has a guaranteed
  teardown (acquire off, lamp off, FIFO reset, DX stop, motor stop, panel
  LEDs idle) that also runs on error, exception and Ctrl+C.
- Live scans (`scan --live-scan`) light the lamp and move film: run them
  only with the operator present. Load film only after
  `corrections: final` (calibration must see an open gate).

## Repository map

```
apps/pakon-cli/       CLI: list / attrib / probe / descriptors / identify / status / eeprom / scan / frames
apps/pakonx/          PakonX desktop app (Tauri 2) over pakon-cli — read-only for now
design/pakonx/        PakonX design system: tokens, components, brand book
src/pakon/
  usb/                IUsbTransport, SetupAPI enumeration, WinUSB
                      backend, identity rules, driver attribution
  bootstrap/          read-only probe (stage-1 personality read)
  ppb/                frame serialize/parse + session client
  scanner/            session state machine + model detection
  protocol/           header-only constants and frame builders (scan registers)
  eeprom/             read-only EEPROM reader with compile-time allow-list
  stream/             concurrent EP6 image stream (queued reads, locked ring)
  scan/               modes, device wrapper, line sync, Corrections, runner,
                      teardown, scan.json sidecar
  image/              PAKRAW writer, TIFF preview
  process/            offline processing (pakon_process): sidecar JSON,
                      .pakraw reader, framing, positives, the OEM look's
                      inputs, DX decode
  errors/ logging/    Result<T> error types; leveled logger + hex dumps
tests/support/        simulated F-135+ (sim_device), pkusb TCP server
tests/                14 CTest entries — no hardware required
tools/                pcap_to_jsonl, compare_sessions, wine/ (OEM stack
                      under Wine against the simulator), oem_proxy/ (XP:
                      logging stand-in for PakonIMAu.dll), oem_host/ (32-bit
                      host for the OEM DLL), re/ (reverse-engineering helpers),
                      pakon_look.py (prototype of --pakon-look)
driver/               PakonWinUSB.inf + signing/install notes
tools/                frozen boot reference (approved runs only)
docs/                 all documentation (below)
```

Layering rules (who may call whom, and what each layer may emit) are in
[docs/ARCHITECTURE.md](ARCHITECTURE.md).

## Documentation — recommended reading order

1. **[docs/STATUS.md](STATUS.md)** — what works, how it is verified,
   ranked risks. Start here. Then **[docs/OEM_RE.md](OEM_RE.md)** (the
   device/protocol reference) and
   **[docs/HARDWARE_RUNBOOK.md](HARDWARE_RUNBOOK.md)** (first scans).
2. **[docs/ARCHITECTURE.md](ARCHITECTURE.md)** — the layers and
   the rules between them.
3. **[docs/BOOT_CHAIN.md](BOOT_CHAIN.md)** — the hardware-validated
   cold→warm boot milestone: exact requests, gate bytes, artifact MD5s,
   per-claim evidence markers.
4. **[docs/LOADER_DESIGN.md](LOADER_DESIGN.md)** — how the boot
   sequence becomes reusable C++ components, and the rollout plan.
5. **[docs/USB.md](USB.md)** and
   **[docs/BOOTSTRAP.md](BOOTSTRAP.md)** — transport decisions;
   the probe's evidence trail.
6. **[docs/PPB.md](PPB.md)** and
   **[docs/SCANNER.md](SCANNER.md)** — protocol layer and session
   layer, both capture-verified.
7. **[docs/PAKON_REFERENCE.md](PAKON_REFERENCE.md)** — how the
   primary external spec maps into this repository, gap by gap.
8. **[docs/WINUSB_TEST.md](WINUSB_TEST.md)** — physical WinUSB
   binding procedure and its evidence table.
9. **[docs/F135_TOPOLOGY.md](F135_TOPOLOGY.md)** — the read-only
   descriptor/topology scan of the booted runtime: method, safety
   envelope, observation record.

## Hardware record (lab unit)

| Run | Result | Record |
|---|---|---|
| WinUSB binding, cold unit | probe opens through interface GUID `{0e9e6f29-…}` | docs/BOOTSTRAP.md § 6 |
| Cold `0xA9` baseline | `win32 121` — boot ROM silent (expected) | docs/BOOT_CHAIN.md § 5 |
| Stage-1 upload → `0xA9` | answered `C0-05-0F-35-F2-07-AA-04` | docs/BOOT_CHAIN.md § 5 |
| **Full boot, 2026-10-07** | all 11 steps OK → `USB\VID_0F05&PID_F135\010-203-04`, `status=OK`, `service=WINUSB` | docs/BOOT_CHAIN.md § 3 |
| **Descriptor scan, 2026-10-08** | `descriptors` — device descriptor byte-identical to the Pakon7 image @ `0x1000`; 1 vendor interface, bulk `0x01`/`0x81`/`0x86` @ 512; strings incl. serial `010-203-04`; interface path carries this repo's GUID | docs/F135_TOPOLOGY.md |
| **First PPB session, 2026-10-08** | `identify` — connect handshake + probes `0x44`/`0x24` answered; module-info reply well-formed (12 B) but `0x88` READ-flags byte rejected by `is_success` → stopped (payload uninterpreted) | docs/STATUS.md |
| **identify complete, 2026-10-08** | after `254e8a1` (`is_read_success`): all 7 exchanges answered — `0x40` module-info `0x88`, `0x44` module-info `0x08`, bridge `0x10` `0x88` → `Model: F-135+`; lab unit's module-info payload differs entirely from the capture corpus (decoded: raw hex only) | docs/SCANNER.md § 3 |
| **status complete, 2026-10-09** | `status` — all 13 exchanges answered; polls revealed the `0x80` event bit on READ_STATUS (`0x88` on HOST/light) and a 6-byte HOST poll form; values: CCD `0x12`, light status `80 02` (= corpus), temperature `82 02 d2 01`; light module-info `[0..3]` changed vs 10-08 (volatile field); device responsive throughout → `is_poll_success()` added | docs/STATUS.md |

Not yet run on hardware: the CLI's `list` command. (`attrib`,
`descriptors`, `identify` and `status` ran 2026-10-08/09 — see
[docs/STATUS.md](STATUS.md) and [docs/F135_TOPOLOGY.md](F135_TOPOLOGY.md).)

## External references

- **Primary spec:** `pakon-reference` (external repository) — mapped in
  [docs/PAKON_REFERENCE.md](PAKON_REFERENCE.md).
- **Capture corpus:** `alibosworth/pakon-captures` — replayed by the
  test suite.
- **OEM artifacts** (fetched + MD5-pinned at run time, never committed):
  the public `plonsker/pakon-scanning-software` mirror of the Pakon
  F-135 driver install.

This repository re-derives and cites; it does not redistribute firmware
and is not affiliated with the OEM.
