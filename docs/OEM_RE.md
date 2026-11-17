# OEM reverse engineering — the device and protocol reference

This is the single reference for how the F-135+ behaves on the wire and how
the official Kodak/Pakon software drives it. Every claim carries one tag:

| Tag | Meaning |
|---|---|
| `[DIS module@addr]` | read in the disassembly / Ghidra decompilation of the OEM binary at that virtual address |
| `[DYN run]` | observed by running the unmodified OEM stack against this repository's simulator (§11) |
| `[CAP file:t]` | in the `alibosworth/pakon-captures` corpus (F-135+ serial 16402), file and time offset from the first command |
| `[BRIDGE file:line]` | in the bridge code that drove the OEM stack on real hardware (`pakon-captures/bridge`, `pakon-tlx-macos`) |
| `[REF doc]` | pakon-reference |
| `[LOG]` | the OEM error logs written on **this** unit (serial 17373) in `psiref/F-X35 COM SERVER/Logs` |
| `[VM run]` | TLB's calls into PakonIMAu.dll on **this** unit, logged by `tools/oem_proxy` in the XP VM (run3 2026-10-14, Base 4; runs of 2026-10-22, Base 8/16), with the OEM's own `Logs\DxCode.txt` of the run |
| `[HW scan]` | a scan of this repository's `pakon-cli` on this unit (`river1`, `roll1`, `strip8`, `strip16`, `strip8dx`; local data, not in the repository) |
| `[INFERRED]` | a reading consistent with the evidence above, not shown by it |
| `[UNKNOWN]` | not established |

Binaries analysed (MD5): `TLB.dll` `193d9b2c…6c0fc` (3.1.0.28, image base
0x10000000), `tlx.dll` `d81bca55…c4994`, `TLXClientDemo.exe` `4d8b2a7e…eaf18`
(base 0x400000), `F135usb2.sys` `05afc8c6…6e12e`, `PakonIMAu.dll`
`eea9dcf7…512242d`. Method: Ghidra 12.1.4
headless decompilation of every function, call sites labelled through TLB's
own error reporter (`FUN_1001acd0(ctx, classId, fnId, errCode, …)`, 751 call
sites) using the id→name tables of `pakon-tlx-macos/server/pknames.py`, plus
capstone for FPU constants. Captures: `pakon-captures` commit `cf41ba6`.

Names below are the OEM's own function names where the reporter gives them
(`bDrvLampOn`, `bCalibrateFindDarkOffset`, …). Older working labels used in
pakon-reference and the bridge are listed where they differ, because several
of them turned out to be wrong (§3).

---

## 1. Layering and the device seam

```
TLXClientDemo.exe ──COM──▶ tlx.dll (TLXMain) ──▶ TLB.dll (CiScanner, CiCmdComm, …)
                                                     │  CreateFileW("\\.\Pakon135")
                                                     │  DeviceIoControl 0x222090  PPB command exchange
                                                     │  DeviceIoControl 0x222059  EP0 vendor transfer
                                                     │  ReadFile (overlapped)      image ring
                                                     ▼
                                               F135usb2.sys ── USB ── FX2 bridge ── PICL / PICM
```

- **Command exchange**: `DeviceIoControl(0x222090, frame, len = frame[1]+2,
  out 64 B)`, overlapped, then `WaitForSingleObject(…, 2000 ms)`. A reply
  whose type is 1 or 3 must echo the request type; 7 is the ACK; anything else
  is `EC_DRV_InvalidPacketType (1004)` `[DIS TLB@0x10008530]` — the 43
  occurrences of that error in this unit's log are exactly this check `[LOG]`.
- **EP0**: `0x222059` with a 10-byte struct (direction, type, recipient,
  reserved, bRequest, pad, wValue, wIndex) `[BRIDGE pakonusb.py:11]`; only
  used for the EEPROM (§5) `[DIS TLB@0x10015d80]`.
- **Image**: one overlapped `ReadFile` over a whole ring (§6).

## 2. What the official client does (TLXClientDemo.exe)

`[DIS TLXClientDemo@…]` throughout; vtable offsets are dual-interface
(IDispatch) slots, method order from the type library
(`PTS/Interop.TLXLib.dll` metadata).

1. **Connect** (`FUN_0040a0d0`): `CoInitializeEx(0, COINIT_MULTITHREADED)`;
   `CoCreateInstance(CLSID_TLXMain, CLSCTX_INPROC_SERVER, IID_ITLXMain)`
   (`@0x404010`); `QueryInterface` for `IScanPictures`, `ISavePictures`,
   `ILongOpsCB`, … (`@0x405170–0x4051ec`); `CBAdvise` registers the client
   callback (`Awake(lOperation, lStatus)`).
2. **InitializeScanner(iInitializeControl, iSaveToMemoryTimeout)**
   (`@0x405223–0x40522f`): `iInitializeControl = 1`
   (`INITIALIZE_ProgressUpdatesAsPercent`), or `3` when the registry value
   `HKLM\Software\Pakon\TLXClientDemo\General\LoadFirmwareAtStartup` is
   non-zero (adds `INITIALIZE_FirmwareUpdate = 2` — the PIC flash path of §4.1
   step 5); `iSaveToMemoryTimeout = 20000`. `MotorSelfTestAtStartup` only sets
   a dialog checkbox (`@0x40a81b–0x40a864`); it is never passed to
   `InitializeScanner`.
3. **Progress** arrives through `Awake(lOperation, lStatus)`
   (`FUN_00409470`, `FUN_004090e0`): `WTO_InitializeProgress (0)` →
   "Initializing Scanner"; `WTO_LampWarmUpProgress (24)` → "Lamp Warm Up";
   `WTO_CorrectionsProgress (20)` → "Corrections"; `WTO_ScanProgress (34)` →
   "Scanning" / "Finished Scanning"; `WTO_SaveProgress (38)` → "Transfering
   Pictures". `lStatus` uses `WTP_ProgressStart 1000 / End 2000 / Complete
   3000`. Warnings: `GetInitializeWarnings` → `INITIALIZEW_EEPROM_BLANK (1)` →
   "EEPROM NOT INITIALIZED", `INITIALIZEW_EEPROM_CHECKSUM_BAD (2)` →
   "EEPROM CHECKSUM_BAD" (`FUN_00407400`).
4. **ScanPictures(iResolution, iFilmColor, iFilmFormat, iStripMode,
   iScanControl, bstrRollId)** (`FUN_00404780`, vtable `+0x2c`), parameters
   built in `@0x408b50–0x408d33` from the scan dialog:
   - `iResolution` = dialog index (`RESOLUTION_BASE_4 0`, `_8 1`, `_16 2`);
   - `iFilmColor`: combo 0→`1` NEGATIVE, 1→`2` POSITIVE, 2→`4` BnW_NORMAL,
     3→`8` BnW_C41;
   - `iFilmFormat`: `0` (35 mm) for the 35 mm choices, else `1`;
   - `iStripMode`: 0, or 4/5/6 for the three strip modes;
   - `iScanControl` = `0x200000` (`SCAN_UseOrderAnalysisCallbacks`) `| 2`
     AggressiveFraming `| 8` UseScratchRemoval (**turns the IR LED on**, §9)
     `| 0x100000` PremiumColorPath `| 0x1000` HasFilmDrag `| 4`
     RFT_SenseSplice `| 0x10` 24 mm loader `| 0x10000` PreScan, per checkbox;
   - `bstrRollId` defaults to `"1000"`.
   **ForceCorrections is never called by the demo**: corrections run inside
   ScanPictures when needed (§7). Lamp Warm Up and Corrections are progress
   phases of ScanPictures, not separate calls.
5. This unit's `EC_InvalidParameter (15) 4 iFilmColor or iScanControl` `[LOG]`
   is `iFilmColor = 4` (BnW_NORMAL) being rejected `[INFERRED from the enum
   value; tlx-macos FILM-MODES.md documents the greyed-out B&W modes]`. It is a
   client setting, not a driver fault.
6. "Adjust Motor Speed?" (`@0x405d48`) offers `IScanPictures::AdjustMotorSpeed`
   after a scan whose `SCANW_MOTOR_SPEED_*` warning is Half/One Percent
   Fast/Slow; that routine rewrites the EEPROM motor-adjust words
   (`bWriteEEPromAdjust`, `FUN_10016f50`) — never reproduced here.

## 3. Register map (as the OEM software names and uses it)

Addresses for the F-135+: HOST `0x10`, PICL `0x40`, PICM `0x44` (F-135:
`0x20`/`0x24`) `[DIS TLB@0x1000afd0]`. CCD-register writes go to the PICM
address `[DIS TLB@0x1000a5d0]`.

| Addr | Reg | OEM function | Payload | Notes |
|---|---|---|---|---|
| HOST | `0x85` CMD | HostReset | — | `[DIS TLB@0x1000b100]` |
| HOST | `0x8F` W1 | HostSetMode | 0 / 1 | 1 only while probing a recovery controller at `0x28` |
| HOST | `0x84` W1 `02` + PICL `0x8A` CMD | **bDrvResetFifos** | — | was "ARM pair / HostReady+AcquireLine" `[DIS TLB@0x1000a730]` |
| HOST | `0x03` R2 | bridge firmware version | `[lo][hi]` | "USB 0x03,0x0F" `[DIS TLB@0x1000b100]` `[LOG]` |
| any | `0x00` CMD | bDrvFindPicController probe | — | present ⇔ reply `07 02 a 00`; two tries `[DIS TLB@0x10008ba0]` |
| PICM | `0x97` W1 `01` | sent once after the PICM answers its firmware-check probe | — | `[DIS TLB@0x1001c3e0]` (CiFirmware::bUpdate) |
| PICL/PICM | `0x03` W1 `01` then `0x07` R12 | **bDrvGetDevInfo** | 12 B info page; firmware version = `(b[2], b[1])` | `[DIS TLB@0x1000a370]` |
| PICL | `0x02` R1 | interrupt status | service byte | §8 |
| PICL | `0x06` W2 | interrupt acknowledge | `[00][status]` (0 → `ff`) | `[DIS TLB@0x1000bdd0]` |
| PICL | `0x80` W1 | **bDrvLampOn/Off** mask | bit0 visible, bit1 IR | `[DIS TLB@0x1002c5f0, 0x1000c4d0]` |
| PICL | `0x81` W5 | LED currents | `[B, Ir, R, 0, G]` clamped to §9 ceilings | `[DIS TLB@0x1002c5f0]` |
| PICL | `0x82` W12 | LED on-times ("exposure"; was "colour matrix") | six u16 `[B, Ir, R, 0, G, base]` | §9 |
| PICL | `0x83` R1 | lamp status flags | bit3 temperatures valid, bit1 lamp stable | `[DIS TLB@0x1000b890]` |
| PICL | `0x84` R2 | lamp temperature setpoint | u16, 1/16 °C | `[DIS TLB@0x1000b890]` |
| PICL | `0x88` R4 | lamp temperatures | two u16, 1/16 °C | `[DIS TLB@0x1000b890]` |
| PICL | `0x87` W2 | written `00 00` by bDrvInitCcd | — | semantics `[UNKNOWN]` |
| PICL | `0x89` W1 | horizontal resample | 1 = 3/4 (Base 8) | `[DIS TLB@0x1002c340]` `[CAP base8.jsonl]` |
| PICL | `0x8B 0x8C 0x8D 0x8F` W4 | **bDrvInitLampTemperatures** | temperature limits | `[DIS TLB@0x1002d190]` |
| PICL | `0xD0` W1 `00`, `0xD1` W1 `01` | TEC, inside bDrvInitLampTemperatures | **literal constants** | `[DIS TLB@0x1002d190]` |
| PICL | `0x90` R30 | DX sensor records (film presence) | §10 | `[DIS TLB@0x10009790]` |
| PICL | `0x91` W3 | **bDrvDxStart** | `[u16 DX word][flag]` | was "SetScanLineParams / trigger" `[DIS TLB@0x1000a7b0, 0x10009bf0]` |
| PICL | `0x92` CMD | **bDrvDxStop** | — | was "EndAcquisition" `[DIS TLB@0x10009da0]` |
| PICM | `0x82` sub `0` | **CCD FPGA control register** | bit0 acquire, bit1 2:1 binning (Base 4), bits5–6 `0x60` set once at init, bit8 IR mode | was "motor speed / run word" `[DIS TLB@0x10029770, 0x10029810, 0x10029860]` |
| PICM | `0x82` sub `4` / `5` | CCD pixel window start / end | absolute CCD pixel | `[DIS TLB@0x1002c340]` |
| PICM | `0x82` sub `6` | integration time (line period) | ≤ `0xFFD` | `[DIS TLB@0x1002c340]` |
| PICM | `0x82` sub `9` | **bDrvSetLed** — front-panel status LEDs | state word | was "mux"; cosmetic `[DIS TLB@0x1000c1f0]` |
| PICM | `0x82` sub `1 2 3 10 11` | written once by bDrvInitCcd (`0,0,0,0x400,0`) | — | `[UNKNOWN]` |
| PICM | `0x84` sub `0`/`1` | `0x78`, `0x80` at init | — | `[UNKNOWN]` |
| PICM | `0x84` sub `2 3 4` | **A/D gains R G B** | 0…`0x3F` | `[DIS TLB@0x100298b0]` |
| PICM | `0x84` sub `5 6 7` | **A/D offsets R G B** | sign-magnitude: `|v|`, `+0x100` = negative, `|v| ≤ 255` | `[DIS TLB@0x100299c0]` |
| PICM | `0xA5` W2 | motor rate | clamped `[1000, 0x7FFE]` on 0x44 boards (`[400, 0x251C]` on 0x24) | `[DIS TLB@0x1000b6d0]` |
| PICM | `0xA0` / `0xA1` CMD | motor go forward / reverse | — | `[DIS TLB@0x1000b6d0]` |
| PICM | `0xA2` CMD | motor stop (bDriveMotorStop) | — | `[DIS TLB@0x1000a440]` |

PSI's **Advance Film / Reverse Film / Stop Advance Film** menu calls
`IScanPictures::AdvanceFilm(300000, flag ? 1000 : -1000)` (vtable `+0x3c`)
and `StopFilmDrive` (`+0x40`) `[DIS PSI.exe@0x4461f1, 0x446230]`. TLB's
`bDriveMotorAdvanceFilm` sends rate `0xA5` = |speed| (clamped `[1000, 0x7FFE]`
on 0x44 boards), then `0xA0` (speed > 0) or `0xA1` (< 0). It then waits for the
time or a stop request and sends `0xA2` `[DIS TLB@0x1000b6d0]`. `0xA1` is in no
capture yet. `pakon-cli film-drive` follows this, with the full teardown, but at
the Base 4 scan rate 25676 instead of 1000 `[CAP river1.scan.json motor_rate]`.
| PICM | `0xA2` CMD | **bDriveMotorStop** | — | `[DIS TLB@0x1000a440]` |

Corrections to earlier labels, by evidence:

- **The image stream is gated by FPGA control bit0, not by `0x91`.** In all
  eleven captured sessions EP6 data starts ≤ 5 ms after a sub-0 write with
  bit0 set and stops ≤ 1 ms after bit0 is cleared `[CAP all files; e.g.
  base4.jsonl run 21.278 → first EP6 21.282, idle 24.787 → last EP6 24.789]`.
  The OEM writes bit0 together with `0x91` (`bDrvCcdAcquireAndDxStart`,
  `[DIS TLB@0x10009bf0]`), which is why the bridge took `0x91` for the
  trigger. `0x91` alone (base4.jsonl t 14.343) produces no data.
- `0x82` sub-0 values `0x60/0x62/0x160` are control words: `0x60` = init
  bits, `0x02` = Base-4 binning, `0x100` = IR mode, `| 1` = acquire.

## 4. InitializeScanner, end to end

The PPB poll thread (`uiDriverPollPPB`, `[DIS TLB@0x1002eb70]`) runs the
initialisation, then becomes the 200 ms status poller (1 ms while scanning).

### 4.1 Order `[DIS TLB@0x1000b100, 0x10028d30]`, bytes `[CAP base4.jsonl 5.629–6.696]`

1. `bDrvOpen`; HostReset `04 03 10 00 85`; HostSetMode 0 `02 04 10 01 8f 00`.
2. Controller probe (`FUN_1000afd0`): `04 03 44 00 00` — present ends the
   search; otherwise `0x46`, `0x24`, `0x26` (the last two give the F-135 pair
   `0x24/0x20`). A probe to `0x46`/`0x26` is a CMD with register `0x00`,
   outside the bootloader-erase command range `0x0C–0x0F` `[REF
   per-unit-data-and-safety.md]`; this stack still never sends it (F-135+ only).
3. **EEPROM → registry** (§5).
4. Bridge version: READ HOST reg `0x03` (2 B).
5. Firmware check per controller (`CiFirmware::bUpdate`, `[DIS TLB@0x1001c3e0]`):
   probe the PICM again; when present write **PICM `0x97` = `01`** once; then
   `bDrvGetDevInfo` for PICL then PICM: **`0x03` = `01`, READ `0x07` ×12**.
   The version pair is `(info[2], info[1])` → "Lamp 0x05,0x0A Motor 0x05,0x06"
   `[CAP base4.jsonl 5.795–5.814]` `[LOG]`.
   With `INITIALIZE_FirmwareUpdate` set and a newer `NLvvff.hex` /
   `NMvvff.hex` in `Config\Firmware`, the same routine erases and rewrites
   PIC flash through the bootloader address (`0x42`/`0x46`)
   (`FUN_1001b810…0x1001bdf0`). **This stack never implements that path.**
6. `bDrvResetFifos` (HOST `0x84`=`02`, PICL `0x8A`).
7. `bDrvInitLampTemperatures`: PICL `0x8F`, `0x8C`, `0x8B`, `0x8D` (4 B each,
   registry temperature limits), optional `0x8E`, then **TEC `0xD0`=`00`,
   `0xD1`=`01` (literals)**.
8. `bDrvInitCcd` (`[DIS TLB@0x1002d5c0]`): PICL `0x87`=`0000`; `bDrvLampOn(vis,
   integration 0xFFD, all durations 0)` → `0x80`=`01` + `0x82` zeros with base
   `0x3D6`; `bDrvLampOff` → `0x80`=`00`; `0x89`=`00`;
   `bDrvPutCcdFpgaSettings(width 2000, start 0x3E, integration 0xFFD)` →
   sub6 `0xFFD`, sub0 `0x60`, sub11 `0`, sub4 `0x3E`, sub5 `0x80E`; then sub1/2/3
   `0`, sub10 `0x400`; `0x84` sub0 `0x78`, sub1 `0x80`.
9. `bDriveMotorStop` (`04 03 44 00 a2`), status reads (`0x90`, `0x83`,
   `0x84`, `0x88`), status LEDs (sub9 `0x14`, `0x17`).

### 4.2 The pre-init writes the old stack skipped

PICM `0x97`=`01`, PICL `0x03`=`01`, PICM `0x03`=`01` precede the module-info
reads in every capture `[CAP all six, e.g. base4.jsonl 5.798/5.802/5.809]`
and are part of the OEM's own sequence `[DIS TLB@0x1001c3e0, 0x1000a370]`.
Without the `0x03` select the `0x07` read returns a different page — which is
why the lab unit's module-info replies never matched the corpus (STATUS.md
history) `[INFERRED, strongly: same request, different page]`.

## 5. The per-unit EEPROM

Read primitive `bEEPromRead` `[DIS TLB@0x100160a0]`: chip `n = 2`, select
`wValue = ((n | 0x50) << 1) | 1 = 0x00A5`; for each ≤ 32-byte chunk:
vendor OUT `0xA4` (wValue `0x00A5`, wIndex `0x1234`, no data), then vendor IN
`0xA9` (wValue = byte offset, wIndex `0x1234`). Offsets must be `< 0x2000`.
The write variant (select bit0 clear, request `0xA2`) lives in the same
routine and is used only by the calibration wizard / AdjustMotorSpeed —
**never by this stack** (compile-time allow-list in `src/pakon/eeprom/`).

Sections `[DIS TLB@0x100163c0, 0x10016a90]`: header `{u32 length, u32 crc32}`
then payload; CRC-32 (reflected, zlib) over the payload. For each section
the primary is read; a length of 0 or above the section maximum is retried
once and then flagged **blank (1)**; a CRC mismatch is flagged **checksum
bad (2)**; on either flag the backup copy is tried; only if both copies fail
does the warning reach `GetInitializeWarnings` and the built-in defaults stay
in force — a warning, not an error. Sections: A at `0x000` (backup `0x400`,
max 398 B), B at `0x800` (backup `0xA00`, max 36 B). The capture reads
exactly that `[CAP base4.jsonl 5.632–5.794]`.

Field map `[DIS TLB@0x10016860, 0x10016610]`:

| EEPROM | Field | Used for |
|---|---|---|
| `0x008` u32 | hardware version | registry `ScannerVersionHw` |
| `0x00C` u32 | scanner type (1350 F-135 / 1351 F-135+) | model cross-check |
| `0x010` u32 | serial | log header "Serial 17373" `[LOG]` |
| `0x014 / 0x01A / 0x020` u16 | **Offset** for Base 4 / 8 / 16 | FPGA pixel-window start in the film pass |
| `0x016 / 0x01C / 0x022` u16 | **MotorSpeed** (no IR) | film-pass motor rate |
| `0x018 / 0x01E / 0x024` u16 | **MotorSpeed IR** | film-pass motor rate with IR |
| `0x026 – 0x09D` f32 ×30 | **NegMatrix**: per output R, G, B the coefficients of R, G, B, R², G², B², RG, RB, GB, 1 on TLB's density (§13); equal to the registry `TLB\ColorKodak\NegMatrix0–29` on serial 17373 `[HW eeprom 2026-10-20]` | stage-2 density (sidecar `unit.neg_matrix`) |
| `0x09E – 0x115` f32 ×30 | PosMatrix (0.25 diagonal on this unit) | slides, not used yet |
| `0x808 / 0x810 / 0x818` u16 ×4 | **MotorAdjust** per base: `[normal, normal+drag, IR, IR+drag]`, clamped 900–1100 | rate = speed × adjust / 1000 |
| `0x820` u32 | `[UNKNOWN]` | — |

Cross-check against the captured unit: base-4 Offset `0x001E`, MotorSpeed
`0x647E`, both readable in the bridge narration of the 32-byte read at
`0x008` `[CAP base4.jsonl 5.640]`; the captured rates `0x647E, 0x4B4E,
0x2CAA, 0x1D85, 0x170C, 0x12E4` equal that unit's six speeds with adjust 1000
`[CAP README] [REF calibration.md table]`. **Every one of those values is per
unit; this stack derives them from the attached unit's EEPROM.**

## 6. The image ring and the scan-driver thread

TLB side (`iScanStrips`, `[DIS TLB@0x10029b80]`; allocator `[DIS
TLB@0x10028af0]`):

- One `VirtualAlloc` buffer: a 0x38-byte control block in the first page, data
  from `base+0x1000`; `N = RingTailDriverBytes (0x800000) / packet` packets;
  observed live `N = 409`, packet 20480, threshold 3 `[BRIDGE pkusb.c:270]`.
- One overlapped `ReadFile` over the whole ring, posted by a thread raised to
  `THREAD_PRIORITY_HIGHEST (2)`; two worker threads consume: `uiGetScanLines`
  (`@0x1002f550`) and the processing thread (`@0x10030c00`).
- The **kernel driver** fills the ring: it keeps `ring[0x10]` IRPs in flight,
  each one bulk-IN URB of one packet (≤ `0x5000`) mapped straight onto the
  next free ring slot, advancing `Writing` (`+0x1c`) modulo N and signalling the
  event at `+0x2c` `[DIS F135usb2.sys@0x11634, 0x11e2c]`. The host never stops
  reading while a transfer is armed — the stream is drained at USB speed no
  matter what the consumer does.
- Overflow: the driver sets `+0x32` when the ring is full and the consumer
  logs `EC_DRV_RingTailOverflow (1002)` `[DIS TLB@0x1002f330]`; a packet with
  no line marker is `EC_DRV_CannotFindStartOfScanLine (1001)` `[DIS
  TLB@0x1002f240]`; `EC_ProcessedRingTailOverflow (149)` is the second
  (processed-lines) ring `[DIS TLB@0x1002f450]`. This unit's scan log has 12
  ×149 `[LOG]`: its OEM host could not keep up with processing.
- Stop: the app sets `StopTransfer (+0x30)`; the driver completes the read and
  clears `TransferInProgress (+0x31)`; TLB then `CancelIo`s and clears the FPGA
  acquire bit.

**Consequence for a WinUSB host**: one synchronous 20 KiB read at a time
cannot reproduce this — between two reads the FX2's small FIFO overflows at
~6.4 MB/s. A reader thread with several overlapped reads queued and a
decoupled consumer ring is the userspace equivalent (`src/pakon/stream/`).

## 7. ScanPictures: the scan state machine

`bBeforeScan` `[DIS TLB@0x1002dbd0]`, `bAfterScan` `[DIS TLB@0x1002a900]`,
bytes `[CAP base4.jsonl 14.301–42.434]`.

1. `bDrvResetFifos`. Corrections are (re)run when the dark-point interval
   (`DarkPointCorrectIntervalMinutes`) has elapsed or full light corrections are
   pending (always on a fresh install); otherwise the stored per-mode values
   are reused — the capture is such a run: no LED servo, currents written once.
2. **Corrections** (`bCalibrateFindCorrections`, `@0x10021590`):
   [exercise steppers] → **Lamp Warm Up** (`bLampTemperatureStable`,
   `@0x1002cf10`): poll every 250 ms, up to 300 s, until the poll thread has
   seen the lamp stable (§8), aborting on hardware-fault mask `0x040548C0` →
   `bCalibrateStartDataFlow` (one ring transfer for the whole calibration) →
   `bCalibrateLEDs` (§9) → `bCalibrateEndDataFlow` (acquire off).
3. Film-pass setup: `bDrvPutCcdFpgaSettings(width, start = EEPROM Offset,
   integration)`, A/D offsets and gains from Corrections, status LEDs,
   `bDrvLampOn` with the per-mode currents and on-times, motor rate =
   `MotorSpeed[IR] × MotorAdjust[IR, drag] / 1000` clamped, `0xA5` + `0xA0`.
4. `iScanStrips` (§6): ring armed, **`bDrvCcdAcquireAndDxStart`**: FPGA
   control `| 1` then `0x91 [DX word][flag]` — the data starts here.
5. The scan ends when the processing side decides the film has passed (§10)
   or on `EC_NoFilmTimeOut`; then acquire off, lamp off, StopTransfer,
   `bDrvResetFifos`, **`bDrvDxStop` (`0x92`)**, **`bDriveMotorStop`
   (`0xA2`)** `[DIS TLB@0x1002a900]` — exactly the captured teardown
   `0206440382006200, 020440018000, 020410018402, 040340008a, 0403400092,
   04034400a2` `[CAP base4.jsonl 38.014–38.088]`.

The OEM's motor stop is the bare `0xA2` once the acquire bit is clear; it never
writes a rate below its clamp (1000 on the F-135+) `[DIS TLB@0x1000b6d0]`.
pakon-reference's "disengage without restoring idle leaves the motor running"
`[REF film-transport.md]` is the acquire bit, not a motor register.

### 7.1 Per-mode constants `[DIS TLB@0x10010760 region, FUN_10011510]`

| Base | width px | resample (`0x89`) | binning (ctrl bit1) | cal. start | integration | integration IR | default Offset |
|---|---|---|---|---|---|---|---|
| 4 | 1000 | 0 | 1 | 3 | 1875 | 1250 | 31 |
| 8 | 2000 → 1500 | 1 | 0 | 6 | 2813 | 2128 | 62 |
| 16 | 2000 | 0 | 0 | 6 | 4093 | 2498 | 62 |

Every value matches the captures `[CAP sub4/sub5/sub6 writes, all six
configs]`. The DX words for `0x91` are `0x0107 / 0x00C5` (Base 4 / IR),
`0x0075 / 0x004D` (8), `0x003C / 0x0031` (16) with flag `0x01` `[CAP all six,
and an independent OEM datalogger capture per the corpus README]`; TLB derives
them in FPU code at the same site, formula `[UNKNOWN]`, so they are replayed.

## 8. Lamp warm-up and the service (interrupt) protocol

`bDrvGetPpbInterruptStatus` `[DIS TLB@0x1000bdd0]`, polled every 200 ms:

1. HOST poll `03 01 10` → `03 03 10 st aa`; bit7 (`0x80`) of `st` = an
   interrupt is pending; bit1 = FIFO overflow note.
2. For PICL, PICM (and `0x28` if present): READ reg `0x02` (1 B). When the
   READ flags carry bit7 (`0x88`), acknowledge with WRITE reg `0x06`
   `[00][status]` (status 0 is acked as `ff`). Captured: `0x02`, `0x20`, `0x22`
   `[CAP base4.jsonl 20.989; extras/base16-ir-nofeed.jsonl 29.158/29.180]`.
3. PICL status `& 0xA4` → DX record read (`0x90`, §10); `& 0x5B` → lamp status:
   READ `0x83` flags, `0x84` setpoint, `0x88` temperatures
   (`bDrvGetHardwareStatusLamp`, `[DIS TLB@0x1000b890]`).
4. Lamp ready: lamp status bit1 moves the warm-up state from "warming" to
   "stable" (CmdComm `+0xd0`, read by Lamp Warm Up as CiScanner `+0x298`)
   `[DIS TLB@0x1000b890, 0x1002cf10]`. In the capture the PICL raises status
   `0x02` 6.5 s after init `[CAP base4.jsonl 20.987]`.
5. Temperatures are 1/16 °C: the lab unit's `0x88` reply `82 02 d2 01` is
   40.1 °C / 29.1 °C and its `0x84` reply `80 02` a 40.0 °C setpoint (resolves
   the STATUS.md "temperature" unknown) `[DIS TLB@0x1000b890 ×0.0625]`.
6. Hardware-fault bits (client formatter `FUN_0040aa40`, in address order)
   `[DIS TLXClientDemo@0x40ab40…0x40af6d] [INFERRED bit-to-text order]`:
   `0x1` Host board, `0x2` DX board, `0x4` Lamp board, `0x8` CCD board, `0x10`
   Motor board, `0x40` Hardware error, `0x100` Lamp warning, `0x200` Lamp
   error, `0x400/0x800` Temperature warning/error, `0x1000` Lamp burn-out,
   `0x2000/0x4000` Lamp fan, `0x40000/0x80000` Power warning/error,
   `0x200000…0x1000000` stepper/filter/guide indeterminate, `0x2000000` Film
   in guide, `0x4000000` Blower, `0x8000000` Lens cleaning, `0x10000000`
   Emulsion down, `0x20000000` Tail first. Bits `0x40000000/0x80000000` are the
   DX film-sensor flags (§10), reported as state, not faults.
   **This unit logs `EC_HardwareFault (135)` with `0x40000100`/`0x80000100`
   405 times and `EC_FilmInGuides (129)` 392 times `[LOG]`**: bit `0x100` is
   also raised by the film-in-guides path while scanning `[DIS
   TLB@0x10009790]` — the OEM software repeatedly saw film where it did not
   expect it on this unit. See the runbook.

## 9. Corrections: the closed loop (`bCalibrateLEDs`, `[DIS TLB@0x10020dc0]`)

All inside one acquisition window; every measurement first calls
`bDrvResetFifos` and averages **32 lines** (`bCalibrateAcquireAndAverageLines`,
`@0x1001d590`) per channel per pixel.

1. **Calibration geometry**: FPGA window `[cal_start, Offset + width)` — it
   includes the masked pixels before the gate (Base 4: 3…1030 = 1027 px →
   3081 samples/line, the bridge's measured 6162-byte line `[BRIDGE capture
   narration]`); integration per §7.1; lamp off.
2. **Dark offset servo** (`bCalibrateFindDarkOffset`, `@0x1001e1c0`): A/D gains
   = gain code(1.2) = 13; offsets start at +10; up to 8 iterations: write
   offsets, average 32 lines, per channel mean over the first `cal_start`
   pixels; converged when `|mean − 300| ≤ 32`; else
   `offset += trunc(−(mean − 300) × 3/112)`. Reproduces the captured ramp
   `+10 → −51 → −43 → −36 → −34` `[CAP base4.jsonl 21.29–21.69]`.
   Gain code: `round((1 − 1/g) × 75.6)`, `g` clamped to [1, 6]
   (`@0x100201d0`) — g 1.2 → `0x0D` `[CAP]`.
3. Fixed-pattern dark (per-column dark reference, host-side).
4. **LED current servo** (`bCalibrateFindLedCurrent`, `@0x1001e7b0`), only when
   full light corrections are due: currents start at 1, on-time fraction 1.0;
   per round: lamp on, average 32 lines, per channel peak over the active
   pixels; while peak ≤ `0xFA00` (R, G), `0xFFDC` (B), `0x9C40` (IR) and the
   current is below its ceiling, current += 1; then each on-time fraction is set
   to `(n−1)/n` (`0.5` if `n < 3`). On this unit the OEM starts the servo at
   on-times 324/180/87 of base 450 (0.720/0.400/0.193 ≈ 1/film boost),
   not at 1.0, so the currents climb until the **boosted film** on-times fit
   (ended R3 G14 B7) `[CAP 17373 oem-base4.jsonl 56.325]`; where TLB takes
   these from is `[UNKNOWN]`. `pakon-cli` starts there too and caps the
   R/G/B on-time at its start value (`scan/corrections.hpp`).
5. **On-time (duty) servo** (`bCalibrateFindLedDutyCycle`, `@0x1001ec90`):
   `duty ← duty × 63968 / peak` (IR: `39968`) until the peak is in
   `[0xF9C0, 0xFA00]` (IR `[0x9C00, 0x9C40]`); a channel that would need
   duty > 1.0 is flagged "needs more light": its current is raised if below the
   ceiling, else its A/D gain code +1 (max `0x3E`), else
   `EC_InsufficientLight (218)` naming the channel.
6. Fixed-pattern bright (host-side per-column gain, formula partly known
   `[REF calibration.md]`), IR lag if IR.

LED on-time register (`bDrvLampOn`, `[DIS TLB@0x1002c5f0]`):
`base = integration × 1e6 / (2 × clk)`, each channel `round(base × duty_c)`
clamped to `base − 2`; payload `[B, Ir, R, 0, G, base]`. The literal clock in
`bSetF135` is 833 333.3 Hz, but every F-135+ capture fits
`base = round(integration × 0.24)` (1875→450, 1250→300, 2813→675, 4093→982),
i.e. an effective 2.0833 MHz `[CAP ×4] [UNKNOWN which config supplies it]`.
LED currents are clamped by `FUN_100203c0` `[DIS TLB@0x100203c0]`:

| Board | IR lit | R | G | B | IR |
|---|---|---|---|---|---|
| `0x44` (F-135+) | yes | 8 | 24 | 24 | 8 |
| `0x44` | no | 4 | 20 | 20 | 0 |
| `0x24` (F-135) | yes | 8 | 8 | 8 | 8 |
| `0x24` | no | 6 | 8 | 8 | 0 |

identical to the bridge's `LED_CEILINGS` `[BRIDGE pakonusb.py:105]`. Each
current increase waits `WaitForLamp_c` (5.0 s) × Δcurrent / ceiling before
measuring (`[DIS TLB@0x1002c5f0]`; registry default `[BRIDGE
base-config.reg]`).

## 10. Line format, film presence, film end

- **Line** `[DIS TLB@0x1001d170]`: 16-bit LE samples; the line starts at the
  sample whose low byte has bit0 set (the marker is pixel 0's red sample);
  pixels are interleaved `R, G, B`; with IR mode the IR lane follows as one
  contiguous block of `N` samples after the `3N` RGB samples. Samples per
  line = `channels × px`, `px = (end − start) × (resample ? 3/4 : 1)`:
  3081/4617/6162 in calibration, 3000/4500/6000 (IR 4000/6000/8000) in the film
  pass `[CAP bridge "line size" narration, all six]` `[REF image-stream.md]`.
- **Film presence** `[DIS TLB@0x10009790]`: DX record header byte flags
  `0x10` → status bit31, `0x20` → bit30; read on PICL service events. Film
  appearing in the scanning states raises `EC_FilmInGuides (129)` + `0x100`.
- **Film start / end**: decided by TLB's processing thread from the scanned
  lines; it signals the panel LEDs at the leading edge (sub9 `0x0295 →
  0x0215`) and at the end (`0x02D4`), and the acquire bit is cleared ~20 ms
  after the end signal `[CAP base4.jsonl 32.122, 37.993, 38.012;
  extras/base8-ir-latefeed.jsonl 60.848, 80.783]`. With no film the window is
  closed after ≈45 s (`i_uiNoFilmTimeOut`) `[CAP extras/base16-ir-nofeed.jsonl
  22.264 → 67.683]`. The detection algorithm itself is `[UNKNOWN]` (processing
  thread `@0x10030c00`, framing code); this stack uses an image-density
  detector against the open-gate level from Corrections, capped by a row budget
  `[INFERRED design]`.

### 10.1 DX records: film code and frame numbers

The DX board reads the film-edge barcode itself; TLB reads its records with
PICL `0x90` (30 bytes) on service events `[DIS TLB@0x10009790]`:

- record = `[u16 BE line counter][entry count][entries of 5 bytes]`; entry =
  `[flags << 4 | type][p0][p1][p2][p3]`; the first entry's high nibble is the
  film-sensor flags (`0x20` film, `0x10`). The counter runs with the line
  clock: on `strip8dx` it is the film row + 34 on every record `[HW scan]`.
- types 5..8 are edge events with a u16 BE position (`p1 p2`, counter
  units); type 5 marks each code on the film, half a frame pitch apart
  (Base 8: ~1185 rows of a 2337-row pitch) `[HW scan]`.
- types 3 / 4 carry the decoded code `[DIS TLB@0x10013cd0, 0x10013f50]`,
  35 mm layout: type 3 frame code `(p1 >> 2) + (p2 & 1) × 64` (> 121:
  − 128), specifier `(p2 >> 1) & 0xF`, product `(p2 >> 6) + (p3 & 0x1F) × 4`,
  valid when `p2` bit 5 is clear and the bit count of `p1`, `p2`,
  `p3 & 0x1F` is even; type 4 product / specifier from `p1 p2` alone. TLB
  keeps the first four distinct product/specifier pairs with a vote count
  and takes the most voted; `DxCode.txt` logs it as "Product = P,
  Specifier = S".
- a decoded code belongs to the latest type-5 mark before it in reading
  order. Codes count half frames: a picture spans codes 2N and 2N + 1 and
  the OEM names it "NA" (Base 8 strip: DxCode frames 39..49 → 19A..24A)
  `[VM run]`. The DX sensor reads ahead of the gate by 0.35 frame pitch
  (860 / 827 / 768 rows of 2337 on one strip) `[HW scan]`.
- checked: the OEM's Base 16 capture decodes to 39/1 (3 votes, 1 parity
  reject) `[CAP 17373 oem-base16.jsonl]`; `strip8dx` to 39/1 = the OEM's
  DxCode.txt for that strip, pictures 19A..24A = the OEM's file names
  `[HW scan] [VM run]`.

Where TLB reads the DX word written with `0x91` from, and the APS layout
(fields one byte earlier, `TLB this+8 == 0`), are not used here.

## 11. Dynamic RE (TLXClientDemo/tlx/TLB under Wine against the simulator)

### 11.1 Setup

The unmodified OEM COM server runs under Wine 11.19 (WoW64) with
`pakon-tlx-macos`'s `pkusb.dll` in place of the kernel driver; pkusb's five
hooked device calls go over TCP to `pakon_sim_server`
(`tests/support/sim_server.cpp`), the simulator of `tests/support/sim_device.*`.
**No scanner is attached and no USB I/O happens.** `tools/wine/oem_client.cpp`
drives `tlx.dll` from a script (interfaces, vtable slots and parameter types
from tlx's type library at run time), `tools/wine/run_oem.sh` runs one
session and logs it in the corpus schema. Prefix steps, in order:

1. copy `F-X35 COM SERVER` + the `*71.dll` runtimes to `C:\Pakon\F-X35 COM
   Server`; `C:\Program Files\Pakon` must resolve to it — TLB loads
   `C:\Program Files\Pakon\F-X35 COM Server\PakonImau.dll` by that literal
   path `[DYN init5: EC_FileNotFound (128)]`;
2. `regsvr32` tlx/TLA/TLB/TLC on the pristine copies, then patch the
   `VERSION.dll` import of the copied TLB.dll/tlx.dll to `pkusb.dll`;
3. `reg import` `anselinstalldir\minilab.reg` and pakon-tlx-macos
   `setup/base-config.reg` into both registry views; create the Ansel
   capability directories under `anselinstalldir\dataPathItems`;
4. PakonIMAu.dll imports `ekjpegi.dll`, `KODAKCMS.dll`, `xerces-c_2_2_0.dll`,
   which are not in this OEM bundle; `tools/wine/make_stubs.py` builds
   loader stand-ins that log any call.

### 11.2 What the runs showed

`InitializeScanner(1, 20000)` against the simulator `[DYN init6]`:

- Order: `HostReset` → `HostSetMode` → probe `0x44` (acked, so `0x24` is
  **never** probed) → **EEPROM read** → bridge version `HOST 0x03` →
  `PICM 0x97 = 01` → per controller `0x03 = 01` + `READ 0x07` (light, then
  motor) → panel LEDs `PICM 0x82 sub9 = 0x0118`. The EEPROM is read right
  after the probe, before the version and pre-init writes.
- EEPROM: read-select `0xA4/0x00A5` before **every** `0xA9` chunk; section A
  primary (8 + 12×32 + 6 bytes) then section B primary (8 + 28) — **the
  backup copy is read only when the primary fails its CRC**, which is why
  unit 16402's capture (corrupt A primary) also reads the A backup
  `[CAP base4.jsonl]`. TLB parsed the simulator's EEPROM as serial 17373,
  hardware version 400 (`GetScannerInfo000`). No `0xA2`, no write-select.
- `Awake` progress: `(0, 0)`, `(0, 1000)` (`WTO_InitializeProgress`
  start), `(1, 3000)`.
- The run stops in `CiScanner::bInit2` initialising PakonIMAu: with the stubs
  the next calls are `KODAKCMS!SpInitialize`, `SpProfileLoadProfile`,
  `SpXformGet`, `SpXformGetChannels`, `SpXformGetDesc` `[DYN init8]` — Kodak
  CMS is genuinely used during scanner initialisation (colour transforms are
  built before any scan). Without the real KODAKCMS.dll the OEM stack cannot
  pass init, so the lamp/TEC init block, Corrections and ScanPictures were
  **not** observed dynamically; those sections rest on `[DIS]` and `[CAP]`.

### 11.3 This stack vs the OEM (compare_sessions)

`tools/compare_sessions.py` of the corpus `base4.jsonl` (OEM, unit 16402)
against this runner's simulated base4 session (`sim_scan_test` with
`PAKON_SESSION_DIR`) — every remaining difference, explained:

| Difference | Why |
|---|---|
| A[0:10] connect/identify absent from B | the runner starts after `Scanner::identify`, which sends them (tests/scanner) |
| pixel end `0x0406` vs `0x0404`, film start `0x1e` vs `0x1c`, motor rate `0x647e` vs `0x620c` | per-unit EEPROM Offset/speed: 16402 (30, 25726) vs the simulated 17373 (28, 25100) — F3 fixed |
| OEM writes `0x81` once (B6 R3 G9) and no LED servo; B ramps currents from 1 | the OEM reused stored Corrections (§7); B runs the full servo (fresh install behaviour) |
| A/D offset and on-time values differ | data-dependent servo results (simulated optics) |
| B's film on-times saturate (0x1c0) | simulated calibration on-times × the film boost exceed the base; on a real unit they follow the measured whites |
| OEM `DX start` (`PICL 0x91`) before warm-up; B after acquire | placement only; B keeps the DX records of the film window for the film code and frame numbers (§10.1) |
| B rewrites `PICL 0x80 = 00` and `0x89 = 00` at the calibration start, and the idle control word once more in teardown | redundant idempotent writes |
| B's tail `rate 0 → go → 0xA2` | the bridge stop sequence this project keeps on every path (§12); both then end with the OEM's panel LED tail `0x0217 → 0x0017` (`0x0017` alone was added after the first hardware film pass left the film-end state `0x02D4` blinking; the first scan after a firmware load still blinked until the next scan, 2026-10-23) |
| EEPROM: B (pakon-cli) always reads both A copies | harmless extra reads; the OEM reads the backup only on a CRC failure (§11.2) |

## 12. Open questions

- The effective LED clock (§9) and which register/config supplies it.
- PICM `0x82` sub 1/2/3/10/11, `0x84` sub 0/1, PICL `0x87` semantics.
- The DX word formula (`0x91`); the film start/end detection algorithm (§10).
- How TLB measures dmin (the film base it passes to Ansel, §13.4); this
  stack uses the 1st percentile of the thumbnails (Base 8: 661/1171/1339
  vs TLB's 632/1128/1326).
- Where TLB's per-line 8-bit framing value is written (`this+0x6c`, §13.2);
  the rule below matches the OEM's runs within 2 bins but is not read in
  the code.
- Whether the bridge's `rate=0 → go → idle` stop does anything the OEM's
  acquire-off + `0xA2` does not (the OEM never sends a rate below 1000).
- The lamp/TEC init block, Corrections and ScanPictures have not been
  observed dynamically (§11.2: needs the real KODAKCMS/ekjpegi/xerces DLLs).

## 13. The processing pipeline (TLB → PakonIMAu.dll)

The scanner delivers linear 16-bit RGB (+IR) lines; everything else is host
processing, done by TLB with the 32-bit `PakonIMAu.dll` ("Pakon look":
Kodak Ansel scene balance + ColNeg colour). This stack keeps the raw output
losslessly (`.pakraw` + `<prefix>.scan.json`: geometry of both windows,
EEPROM values incl. the NegMatrix, A/D offsets/gains, LED currents and
on-times, per-pixel dark and open-gate references, film start/end, DX
records) and reproduces the pipeline offline in `pakon_process`
(`frames.hpp`, `look.hpp`, `dx.hpp`) and `pakon-cli frames`.

### 13.1 Route

`tools/oem_proxy` is a logging stand-in for PakonIMAu.dll in the XP VM
(original renamed `PakonIMAu_real.dll`): every call, argument values, the
memory they point to, and the full image buffers of the first two
full-size frames of a session. `tools/oem_host` is a 32-bit host that loads
the OEM DLL on Windows 11 (WoW64) from a copy of the OEM install and runs a
script of calls; with the captured descriptors it replays TLB's calls
**byte-identical** (ColorAdjust, ScaleAndRotate, the Ansel roll analysis
and balance; Base 4 and Base 8) `[VM run]`. Exports are cdecl; TLB fetches
them by name.

### 13.2 Framing `[DIS TLB@0x100072c0]`

Per film window, on bins of 4 / 6 / 8 lines at Base 4 / 8 / 16:

1. line value = mean over the full width of R, G, B as TLB's 8-bit code,
   `floor(code14 / 64)` with the open gate at 16000 (§13.3); bin metric =
   255 − mean of its lines `[DIS @0x10006870]` (the 8-bit rule: strip8 runs
   vs DxCode.txt within 2 bins `[HW scan] [VM run]`);
2. start threshold from the bin histogram: first peak (the film base), then
   where it falls below 1/10 `[DIS @0x10005d20]`; raised in steps of 2 while
   over-long runs remain and the count of nice runs does not drop, then
   lowered the same way while short runs remain;
3. nice runs (95–115 % of 375 bins) place a 375-bin frame at
   `start + (width − 375) / 3`, signed `[DIS @0x10006930]`; a run of two
   frames places two;
4. gaps between nice frames get `distance / 400` (rounded down when the
   rest is under 100) evenly spaced frames `[DIS @0x100063d0]`;
5. from the last / first nice frame towards the film end / start, the next
   frame goes where a 375-bin window covers most ones within 25 bins of
   play, ties to the middle `[DIS @0x10006ae0, @0x10006ca0, @0x100064e0]`;
   a remaining piece of at least half a pitch becomes a partial frame.

Frame length 375 bins (1500 / 2250 / 3000 lines) and pitch 400 bins are
the `DxCode.txt` Target and its steps in every base `[VM run]`. Not
reproduced: the DX frame cross-check `[@0x10006310]` and the "no film
here" test `[@0x10006630]`. Result: river1 frame starts within 4–8 lines
of the OEM's (registered by the Ansel thumbnails, NCC ≥ 0.998); strip8 /
strip16 within 6–15 lines, except one picture whose run has a one-level
dip that the OEM split and placed "between" `[HW scan]`.

### 13.3 TLB's density (the input of PakonIMAu)

Per channel, R taken s lines earlier and B s lines later than G
(tri-linear CCD: s = 4 / 6 / 8 at Base 4 / 8 / 16), CCD pixel x − 1:

- `t = (raw − dark) / (white − dark)` with the sidecar references
  (Base 8: both windows resampled 3/4, so the reference shift is
  `(film.start − cal.start) × 3/4`);
- 14-bit code `trunc(16000 t)`, clamped to 16383 (gain `125 × 2^32 / span`
  `[DIS @0x1001f550]`);
- `D = LUT[code] = trunc(−3500 log10(code / 16383))`, `LUT[0] = 16383`
  (`fldlg2 / fyl2x` `[DIS @0x1000dfc0]`; the disabled
  `_ClientColNegLut.txt` holds the same table);
- `out_c = NegMatrix_c · (R, G, B, R², G², B², RG, RB, GB, 1)` on D,
  rounded to u16 `[DIS @0x1000cf80]`. NegMatrix = EEPROM `0x026` (§5) =
  registry `TLB\ColorKodak\NegMatrix0–29`; its constants (173/447/623 on
  serial 17373) are also PIBegin settings `+0x60/+0x68/+0x70` as integers.

River1 frame 1 vs the OEM's own input, no free parameter: bias ≤ 2, rms
40 per pixel (the grain of two scans; 8 on 16 × 16 blocks); a free fit of
scale and offsets gives the same rms. Base 8 frame 2: bias ≤ 2.3 with
s = 6 `[HW scan] [VM run]`.

### 13.4 Call sequence per roll `[VM run]`

1. `PIBegin(install_dir, 0, 0, settings*, …)`;
   `PIAnselStartNewRoll(roll, 1)` (1 = CN-Enhanced; 0 DC-Premium,
   3 CN-Lockbeam, 4 CP-Balance); roll `+0x18/+0x1C` = the bin (4/6/8);
2. per frame: the thumbnail, 250 × 375 in every base = the d × d box mean of
   the frame (d = 4/6/8) rotated 180° (`PIScaleAndRotatePlanar`);
   `PIAnselAddScene(roll, scene, desc)`, desc `{buf, 0, 250, 375, …,
   +0x48 source type 1, +0x54/58/5C dmin R/G/B, +0x60/64 DX product /
   specifier}` — **Ansel balances by film type** (§10.1; −1/−1 = not read
   gives a different balance);
3. `PIAnselEndRoll(roll, 1)`, `PIAnselAnalyzeRoll(roll)`;
4. per frame: `PIAnselColorSceneBalancePlanar(roll, scene, {in, out, w, h})`
   on the density frame (Base 4 / 8 / 16: 1000 × 1500 / 1500 × 2250 /
   2000 × 3000); `PIScaleAndRotatePlanar` rotate 270 to 3000 × 2000 /
   2250 × 1500 (not upscaled) / 3000 × 2000; `PIColorAdjustPlanar(desc{in,
   out, w, h, +0x10..0x28 user adjustments, +0x48 mode, +0x4C toning,
   +0x50 saturation profile})` → 8-bit planar;
   `PISaveFilePlanar_8(buf, w, h, 3, path, 0 = JPEG, 90, 300 dpi, 0)`
   `[DIS PakonIMAu@0x100062b0]`.

`PIColorCorrectColNegPlanarScan` is never called: the negative → density
step is TLB's own (§13.3). One template set (the Base 4 capture) serves
every base once roll bin, AddScene height / dmin / DX and the balance and
colour-adjust sizes are poked: the OEM's Base 8 frame then replays
byte-identical `[VM run]`. `pakon-cli frames --pakon-look` builds the
density frames and thumbnails, writes the script and runs `oem_host`;
PakonIMAu writes the JPEGs (river1 and strip8: the OEM's pictures by eye,
JPEG structure equal to the OEM's).

### 13.5 Still open

- **Digital ICE is `DMLDICELib.dll`**, self-contained (imports KERNEL32
  only): `DMLDICEBegin(unsigned long, DICEInfoStaticTag) → handle`,
  `DMLDICEProcess(handle, DICEInfoDynamicTag)`,
  `DMLDICEDefectCount(handle, DICEInfoDynamicTag, unsigned long*,
  unsigned long*)`, `DMLDICEEnd(handle)`, `DICEVersion()` (MSVC-mangled,
  cdecl). The tag structs are passed by value; their layout is `[UNKNOWN]`
  (recoverable from TLB's call sites). It needs the IR plane captured in the
  same pass — `UseScratchRemoval` (`iScanControl | 8`) is what turns the IR
  LED on (§9), i.e. the `-ir` modes here.
- Slides (ColRev, PosMatrix at EEPROM `0x09E`) are not covered.
- **B&W** (`iFilmColor = 4`, BnW_NORMAL; TLXClientDemo greys it out, the
  AutoIt helper TLX_ScanEnable only re-enables the Positive / B&W / Base 16
  radio buttons, controls 1015/1016/1020): TLB feeds PakonIMAu the same
  three-channel densities and calls as for colour negative; only roll
  (`PIAnselStartNewRoll`) `+0x20` = 4 (colour 1), scene balance descriptor
  `+0x48` = 4 (colour 1) and colour adjust descriptor `+0x4c` = 1 (colour 0)
  change; DX −1/−1 on that strip. Replaying the captured roll (33 Ansel
  thumbnails, picture 2's density) with the three values gives a JPEG
  byte-identical to the OEM's `01.jpg` `[VM proxy bwref, Base 8, 2026-10-23]`.
  `frames --bw` / `look_script(..., bw)`.
- dmin and the per-line framing value: §12.
