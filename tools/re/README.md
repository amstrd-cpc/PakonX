# Reverse-engineering helpers

Small stdlib-only scripts behind the stage-2 findings in
[docs/OEM_RE.md](../../docs/OEM_RE.md) §13. They read our own scans
(`.pakraw` + `.scan.json`) and the dumps written by
[tools/oem_proxy](../oem_proxy) in the XP VM. None of the OEM binaries,
their decompilation or the captured dumps are in this repository.

| Script | What it does |
|---|---|
| `ExportAll.java` | Ghidra headless post-script: decompiled C, imports, exports and strings of a program (`analyzeHeadless <proj> <name> -process <bin> -noanalysis -readOnly -scriptPath tools/re -postScript ExportAll.java <out dir>`) |
| `pe_exports.py` | imports and exports of a 32-bit PE DLL |
| `bin_rec.py`, `bin_extract.py` | print / extract one dumped argument from `PakonIMAu_proxy.bin` (by call sequence, argument, phase) |
| `stats.py` | per-plane statistics of a planar u16 or u8 dump |
| `register.py` | find an OEM planar density dump on our scan (line offset, direction) |
| `fit.py`, `fit2.py` | least-squares map from our density to TLB's input density (3x4, 3x10); the source of the matrix in `tools/pakon_look.py` |
| `negmatrix_test.py` | TLB's density model (EEPROM NegMatrix on the 3500-scale LUT density) against an OEM PakonIMAu input dump; optional fixed scale and offset |
| `thumb.py` | relate an Ansel thumbnail to its full frame (4x4 box, orientation) |
| `reg_thumbs2.py` | the same for any base (thumbnail box d = 4 / 6 / 8) |
| `runs_cmp.py` | our framing runs at several thresholds vs the OEM's DxCode.txt runs (metric variants) |
| `dens_align.py` | TLB density model vs an OEM dump: line / pixel offset and R/B shift search |
| `proxy_sessions.py`, `proxy_rolls.py` | split an appended proxy log into sessions; per roll the geometry of every call |
| `reg_thumbs.py` | first line of each OEM frame on our scan, by matching the Ansel thumbnails (the river1 targets of the OEM framing) |

Run with `python3 -I <script> ...`; each script's docstring gives its arguments.
