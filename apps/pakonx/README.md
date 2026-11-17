# PakonX — desktop app

A [Tauri 2](https://tauri.app) shell around `pakon-cli`, styled with the
PakonX design system in [`design/pakonx/`](../../design/pakonx/README.md).
The scanner stack stays in C++; this app runs the CLI and shows what it
prints.

```
apps/pakonx/
  web/                  the UI: plain HTML/CSS/JS, no bundler
    index.html app.css app.js
    ds/                 GENERATED from design/pakonx (gitignored)
  scripts/sync-design.mjs   design/pakonx → web/ds (tokens.json → tokens.css)
  src-tauri/            Rust: window + the pakon-cli bridge (src/cli.rs,
                        src/scan.rs, src/roll.rs, src/pictures.rs)
```

## The window

Three tabs on the app bar (Ctrl+1/2/3):

- **Scan**: the Scan film window (order or batch, resolution, film end, the
  green Scan) and the Scanner window (state, Initialize, Advance;
  Find / Identify / Status under *Device tools*). A finished scan opens on
  the Roll tab.
- **Roll**: the contact sheet, strips of 6 on film. Click selects;
  Ctrl/Shift-click marks several; double-click or Enter opens the frame in
  Light table. **Size** sets the largest frame; six always fit across.
  *Roll settings* on the right holds what applies to the whole roll: film,
  dust removal (ICE), frame format, Develop with, framing and Save.
- **Light table**: one frame on the loupe, the film strip under it, and
  *Picture* on the right (facts, look sliders, Save picture). Esc goes back
  to Roll.

Roll and Light table share the Rolls pane on the left and one selection.
The log is a drawer: **Log ▴** in the status bar opens it, and the status bar
always shows its last line. It opens by itself while a scan runs.

(Older sections below say "Frame panel" and "Look panel": those controls now
live in *Roll settings* and *Picture*.)

**Scanner start-up.** When PakonX opens, the Scanner window shows
"Initializing" while `src-tauri/src/boot.rs` brings the scanner up: it
checks that the XP VM is not running, runs the boot script below in the
background, then runs `identify` (retried for up to 20 s while the scanner
re-enumerates). An F-135+ shows **Ready** with its model and firmware. On
a scanner that is already warm the script finds no cold interface at its
STEP0 and ends there, before any USB request. **Initialize** runs
it again (after plugging the scanner in, or shutting the VM down).

The boot is the frozen `tools/pakon_boot_reference.ps1`, embedded in the
app at build time and run hidden in PowerShell, with one block swapped:
instead of downloading `F235Ldr.sys` and `Pakon7.hex` from GitHub it
copies them from the app's firmware folder. Everything else, the MD5 gates
and all USB code included, runs as frozen; a test pins that the download
is the only difference. No network is needed. Its writes are volatile:
unplug and replug restores the cold state. The last start-up's result and the
script's output are written to
`%LOCALAPPDATA%\app.pakonx.desktop\logs\init.log`.

**Firmware files.** Put the F-135 pair in `src-tauri/firmware/`
(gitignored, never committed): `F235Ldr.sys` (MD5
`6be781bad6fb9cf7ade4095baed336e0`) and `Pakon7.hex` (MD5
`07f5001a951c4be179008d948be82b69`; the FX35Driver copy is a different
file and fails the gate). `tauri.conf.json` bundles that folder into the
installer (`bundle.resources`), so builds fail without it. At run time the
app looks in `$PAKON_FIRMWARE`, then the bundled `firmware/`, then
`firmware/` beside the executable.

**Scan film** (Scanner window) works in orders, as PSI: **New order**
asks for the roll number from the roll's sticker and makes
`<base folder>\<roll number>` (base folder in **Settings**, app bar;
default `Pictures\PakonX`). The scan goes there (`1111.film.pakraw`), its
pictures are named `1111_19A.jpg` and saved there by default. **Scan**
without an order asks for one, then checks the scanner itself; the new
roll then opens here. An order is used for one scan.
See Hardware safety below for what it checks and how Stop works.

**Open** shows every supported file: a `<name>.film.pakraw` (with its
`<name>.scan.json`) opens the roll, JPEG, TIFF and BMP files open as
pictures. The rolls in that folder are listed on the left. A roll opens
with the look chosen in **Settings → Development** (the OEM look by
default: one `frames --pakon-look` run straight from the scan; else
`frames <prefix> --preview` for PakonX's own positive) and shows the
pictures as the OEM frames them, named by the film's DX frame numbers
(19A, 20A, …), with the roll's DX film code. **Develop with** switches
look; the other one is developed the first time it is chosen.
**Roll 180°** is for film fed tail first: a tail-first picture is exactly
the head-first one turned half round, so the screen turns it at once,
nothing is developed again, saves pass `--tail-first` (the OEM look's
JPEGs are turned), and the choice is kept per roll. **Previous** / **Next** (or the
arrow keys) step through the pictures. A roll opened before opens from the
cache at once, unless the scan or pakon-cli changed since; developing
and saving show a progress bar counted in pictures, with Cancel.

**Save picture** / **Save roll** (Frame panel) write the stage-1 positive
of the selected picture (or the frames Ctrl/Shift-clicked in the strip),
or of every picture, into a folder you choose, as JPEG (quality 85-100)
or 16-bit TIFF, through `pakon-cli frames <prefix> --save jpeg|tiff
--quality Q [--dx-names] [--pictures LIST] [--tail-first]`. Files are named
roll + film number, `1111_19A.jpg` (the roll number from the sticker,
asked when scanning, then the film's DX frame number; a picture without
one gets its place on the roll, `1111_05.jpg`), or by picture number,
`1111.frame01.jpg` / `.frame01.pos.tiff`. **Clear** (light table) closes
the roll and goes back to how PakonX opens, initializing the scanner
again. After a whole
roll is saved, the app asks whether to delete the scan (`.film.pakraw`,
`.film.tiff`, `.scan.json`, `.scan-stats.txt`) to free its space; it only
deletes when every saved picture is on disk, and then shows the saved
pictures.

**Open in Negative Lab Pro** (Save panel, shown only when Lightroom
Classic is installed) writes the picked pictures, or the whole roll, as
flat-fielded linear 16-bit negatives (`frames --save negative --dx-names`,
`1111_19A.neg.tiff`) into `<roll>_negatives` beside the scan and opens them
in Lightroom Classic for import. Run Negative Lab Pro there with the
source set to Scanner - Linear.

**⟲ 90° / ⟳ 90°** turn the selected picture, or every frame
Ctrl/Shift-clicked in the strip, a quarter turn. The turn is kept with the
picture's look in `<roll>.adjust.json` (`"rotate": 90`), shown on the
loupe and the thumbnails, and baked into saved files (pakon-cli turns the
16-bit picture before writing; OEM-look JPEGs are turned and re-encoded at
quality 95). Reset and Apply to all change the tone only; each picture
keeps its own turn. Look sliders redraw once per screen frame, only the
picture being changed.

**Framing (whole roll)** moves every frame window along the film when
pictures come out cut off at one edge. Slide **Offset** (in film lines;
right moves the pictures right inside their frames, as you see it) and
the loupe slides with it at once; **Apply** stores the shift as
`"shift"` in `<roll>.adjust.json` and develops the roll again with
`frames --shift N`. Saving, the 100 % view and the OEM look all use the
same shift; the previews of each shift are cached separately.

**Names: Your name + number** saves as `<Name><number>` numbered by the
picture's position on the roll from **First number**, zero-padded to
**Digits** (`Trip_001.jpg`, `Trip_002.jpg`, …), the same number whether
the whole roll or a few pictures are saved (`frames --name BASE --start N
--digits D`). Names are printable ASCII without `<>:"/\|?*`.

Click the picture to see it at 100 % (one image pixel per screen pixel;
drag to move, click or Esc to fit). The full-size picture is made once
per picture (`frames --save jpeg --quality 95` into the cache) and kept.

**Look** (Frame panel) sets exposure, red/green/blue balance (in stops)
and contrast per picture, with Reset and Apply to all. On screen the
look is an SVG lookup table over the previews, computed with the same
formula pakon-cli uses (`process/positive.hpp`, `Adjust`), so what you
see is what gets saved (within 1 of 255 levels, measured). Looks are kept
beside the scan as `<roll>.adjust.json` and passed to saves as
`--adjust`; deleting the scan deletes them.

**Film presets** (Look, PakonX look): **Save film preset** keeps the
selected picture's look for the roll's DX code (product / specifier, e.g.
39/1) in `dx-presets.json` in the app's config folder. A roll of that film
that opens with no PakonX look of its own gets the preset on every
picture; **Apply film preset** puts it on an already graded roll. Saving a
neutral look removes the film's preset. Scans without a DX read take no
preset.

**Film kind** (New order) goes to the scan as `scan --film`: C-41,
C-41 without orange mask, E-6 slide, B&W silver, B&W C-41
(`src/pakon/scan/film.hpp`). Films without a mask scan at the open-gate
on-times instead of the boosted colour-negative ones; clear-base films
get the long film-end wait; B&W silver refuses IR modes. The scan records
it in `<roll>.scan.json` (`"film"`), and an E-6 roll develops without
inversion (the OEM look refuses slides, so it opens with PakonX's look).

**Scan**: the film end stops the pass. The row cap is a safety bound only
(100 000 / 150 000 / 220 000 rows at Base 4 / 8 / 16). **Film end** chooses
how the end is found: light plus DX sensor (default), light only, or none
(the pass runs to the cap).

**Workflow** (New order): *Single roll*, or *Batch*: one folder
`<base>\<batch name>`, rolls named from a template (`{batch}`, `{n}` =
001…, `{date}` = YYYYMMDD; default `{batch}_{n}`) from a start number.
Each roll moves the batch on to the next number and is hot-started from
the roll before it. A taken name is skipped. The open batch survives a
restart until **Finish batch**.

**Advance** (Scanner panel): **<** (backward) / **>** (forward) move stuck
film at the Base 4 scan rate until **Stop** (at most 60 s; lamp off),
as PSI's Advance / Reverse Film (`pakon-cli film-drive`).

**Hot start**: the previous roll's corrections are reused when the scanner, mode and a white
check (within 3 %) still agree and it is at most 10 minutes old; otherwise
the scan calibrates in full.

**Dust removal (ICE)** (Look panel, IR scans only): Off / Light / Normal /
Strong (`frames --ice`, `process/ice.hpp`), kept as `"ice"` in
`<roll>.adjust.json`. Applies to both looks. Silver B&W is left as scanned.

**Frame format** (Framing panel): Full (24x36), Half (18x24) or Auto
(`frames --format`), kept as `"format"`. The OEM look frames 24x36 only.

**Film: Colour negative / Black & white** (B&W kinds set this; the Look
panel changes it for a roll already scanned) is kept with the roll (`"film": "bw"` in
`<roll>.adjust.json`) and passed as `frames --bw`: the OEM look develops
B&W exactly as the original software does (TLB's BnW_NORMAL; a replay is
byte-identical to the OEM's own B&W JPEG), PakonX's look turns grey.
Changing it develops the roll again; each film has its own cache.

**Free software**: PakonX is GPL-3.0 (`LICENSE`); nothing is locked,
nothing phones home except the update check against GitHub releases.

**Develop with: OEM Pakon look** develops the roll with the OEM's own
PakonIMAu.dll (`frames --pakon-look`, Windows) through the 32-bit
`oem_host.exe`. The app carries everything it needs: put them in
`src-tauri/oem/` (gitignored, never committed) and `tauri.windows.conf.json`
bundles the folder into the installer:

```
src-tauri/oem/install/       copy of the OEM install (PakonIMAu.dll, its DLLs, Config, anselinstalldir)
src-tauri/oem/capture/       TLB's captured descriptors (settings.bin, …; this PC: pakon-host/capture/run3)
src-tauri/oem/oem_host.exe   tools/oem_host/out/oem_host.exe (build.bat, 32-bit)
```

On first use the app copies `oem/install` into
`%LOCALAPPDATA%\app.pakonx.desktop\oem-install` and recreates the empty
folders the DLL needs (`anselinstalldir\anselCacheDir`, …; the bundle
drops empty folders, and PIBegin then fails with -20): the DLL writes its
logs and Ansel cache inside its own folder, which Program Files does not
allow. It copies again when the DLL or PakonX's version changes.

Nothing needs setting up: **Settings** (app bar) holds the orders' base
folder, the theme, which look rolls open with, the OEM look files and
About. Its "OEM look files" show them as Bundled and can point to other
folders or an EEPROM dump (only for scans older than `unit.neg_matrix`
in the sidecar); **Use bundled** goes back, and **How it works** explains
it all in a pop-up. Choices made before this version are dropped, so
every install starts on the bundled files. `$PAKON_OEM_HOST` still
overrides the host. In this mode the light table shows and saves the
OEM look, with its own look sliders, turns, Roll 180°, framing and names.
Saving: JPEG 90 with nothing changed copies the OEM's own JPEG; TIFF (8-bit),
another JPEG quality, a look or a turn start from the picture's 8-bit RGB
before the OEM's JPEG step (`frames --pakon-look --look-raw`), so there is
no second JPEG loss.

Loose pictures (8- or 16-bit) are decoded by the `image` crate into 8-bit
JPEG previews in the app cache; the files themselves are only read.

Browser preview without Tauri: put sample data in `web/demo/`
(`rolls.json`, `<roll>/roll.json`, the BMPs; gitignored), serve `web/` and
open `index.html?demo&roll=<name>`.

## Run it

Needs Node 18+, Rust (stable) and, on Windows, the WebView2 runtime
(preinstalled on Windows 10/11). Linux also needs `libwebkit2gtk-4.1-dev`.

```sh
cmake -S ../.. -B ../../build && cmake --build ../../build   # builds pakon-cli
cd apps/pakonx
npm install
PAKON_CLI=../../build/apps/pakon-cli/pakon-cli npm run dev   # PowerShell: $env:PAKON_CLI="..\..\build\apps\pakon-cli\Release\pakon-cli.exe"
npm run build                                                # installer in src-tauri/target/release/bundle/
```

### Other platforms

| Platform | Built where | Output |
|---|---|---|
| Windows x64 | this PC, `npm run build` | NSIS installer |
| Windows ARM64 | this PC, `tools/build-windows-arm64.ps1` (needs the VS ARM64 build tools) | NSIS installer |
| Linux x64 | GitHub Actions, `.github/workflows/desktop.yml` | `.deb` (installs the udev rule) and AppImage |
| macOS (Apple Silicon + Intel) | GitHub Actions, same workflow | universal `.dmg`, ad-hoc signed |

On Linux and macOS pakon-cli talks to the scanner through libusb and boots
a cold scanner itself (`pakon-cli boot`, the frozen script's requests
ported to C++; not yet run on hardware). The OEM look is Windows only.
The AppImage cannot install the udev rule: copy
`src-tauri/linux/60-pakon.rules` to `/etc/udev/rules.d/` once. The macOS
build is not notarized (that needs a paid Apple developer account): the
first time, right-click PakonX > Open, or run
`xattr -dr com.apple.quarantine /Applications/PakonX.app`.

The app finds `pakon-cli` from `$PAKON_CLI`, else next to its own
executable, else on `PATH`.

## The installer (end users)

`npm run build` makes one file,
`src-tauri/target/release/bundle/nsis/PakonX_<version>_x64-setup.exe`.
It installs for all users (Program Files, asks for administrator once)
and the user then just opens PakonX: the scanner is booted and found on
start, the OEM look works, nothing to set up.

It carries:

| What | From | Lands in |
|---|---|---|
| PakonX.exe (static C runtime) | `src-tauri` | install folder |
| pakon-cli.exe (static C++ runtime) | `build/apps/pakon-cli/Release` | beside PakonX.exe |
| firmware (F235Ldr.sys, Pakon7.hex) | `src-tauri/firmware/` | `firmware\` |
| OEM look (install copy, capture, oem_host.exe) | `src-tauri/oem/` | `oem\` |
| WinUSB driver package + scripts | `driver/` + `src-tauri/installer/` | `driver\` |

After copying, `installer/hooks.nsh` runs `driver\install-driver.ps1`
(elevated): it creates a code-signing certificate for this PC, trusts it
(LocalMachine Root + TrustedPublisher), signs `pakonwinusb.cat` with it,
deletes its private key at once (nothing else can ever be signed with
it), and runs `pnputil /add-driver PakonWinUSB.inf /install`. The package
holds no driver binary: it binds the scanner to Microsoft's own
`winusb.sys`. No test mode, no network. Its log is
`driver\install.log`; a failure shows a message and the app still
installs. Uninstalling removes the driver package and the certificate.
WebView2 is part of Windows 10/11; on a PC without it the installer
fetches it.

Before `npm run build`:

1. Build pakon-cli Release: `cmake --build build --config Release`
   (from the repo root).
2. `driver/pakonwinusb.cat` must exist (made once with `inf2cat`, see
   `driver/README.md`; gitignored). The installer re-signs it per PC.
3. `src-tauri/firmware/` and `src-tauri/oem/` filled (above; gitignored).

The firmware and the OEM files are Kodak/Pakon's: check that you may
redistribute them before handing the installer out.

## Hardware safety

The general bridge only runs `list`, `attrib`, `descriptors`, `identify`
and `status` (allow-list in `src-tauri/src/cli.rs`, pinned by its test), plus
`frames <prefix> --preview` and `frames <prefix> --save jpeg|tiff` on scans
on disk (`src-tauri/src/roll.rs`), which never open the scanner; their
arguments are built in Rust from a checked roll path, fixed flags, a
checked format, picture numbers and an existing folder, pinned by tests.
Opening pictures runs no CLI at all. Deleting a scan is a file
operation in Rust, after a confirmation, and refused unless every saved
picture exists and is not empty.
`scan --live-scan` lights the lamp and moves film. It has its own bridge,
`src-tauri/src/scan.rs`, built on
[docs/HARDWARE_RUNBOOK.md](../../docs/HARDWARE_RUNBOOK.md):

- **Scan** runs the read-only checks in order and refuses the scan at the first failure,
  naming it: the XP VM is not running (`vmware-vmx.exe`; its USB
  autoconnect takes the scanner), `list` finds a warm scanner, `identify`
  reports an F-135+, `status` answers, `eeprom` reads without FALLBACK
  values. Only then does it run exactly
  `scan --live-scan --config <mode> --film-rows <cap> --out-prefix
  <folder>/<name> --stop-on-stdin`: the mode from a fixed list, the cap
  from the strip / whole-roll table (1 000..200 000), the name 1-40 letters,
  digits, `-` or `_`, the folder an existing one. It never overwrites the
  files of an earlier scan, never runs twice at once, never during start-up
  and never beside `frames` (developing could starve the USB stream). All
  of it is pinned by tests in `scan.rs`.
- **Stop scan** during the checks cancels the scan before pakon-cli
  starts. During the film pass it writes `stop` to pakon-cli's stdin. pakon-cli handles it
  like Ctrl+C: the pass ends at the next bounded point and the teardown
  (lamp off, motor stop) runs. The process is never killed. If PakonX
  exits mid-scan, the pipe closes and pakon-cli stops the same way.
- The progress bar counts film rows against the cap
  (`progress: film rows N` lines); film end normally ends the pass sooner.
  A finished scan opens on the light table. If the teardown was not
  acknowledged, the app says so in red: look at the scanner.

## Changing the look

Edit `design/pakonx/` (tokens, component CSS/JS, guidelines), never
`web/ds/`. `npm run dev` re-syncs on start; `npm run sync-design` does it
by hand.
