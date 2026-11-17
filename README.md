<div align="center">

<img src="apps/pakonx/src-tauri/icons/icon.png" width="128" alt="PakonX">

# PakonX

**Free, open-source scanning software for the Kodak / Pakon F-135+ film scanner.**<br>
Scan, develop and save your film on Windows, macOS and Linux, without the old XP software.

[![Release](https://img.shields.io/github/v/release/amstrd-cpc/PakonX?style=for-the-badge&color=4c8eda&label=release)](https://github.com/amstrd-cpc/PakonX/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/amstrd-cpc/PakonX/total?style=for-the-badge&color=44cc11)](https://github.com/amstrd-cpc/PakonX/releases)
[![License](https://img.shields.io/github/license/amstrd-cpc/PakonX?style=for-the-badge&color=blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/amstrd-cpc/PakonX?style=for-the-badge&color=f5c542)](https://github.com/amstrd-cpc/PakonX/stargazers)
<br>
![Windows](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D6?style=flat-square&logo=windows&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-Apple%20Silicon%20%7C%20Intel-000000?style=flat-square&logo=apple&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-deb%20%7C%20AppImage-FCC624?style=flat-square&logo=linux&logoColor=black)

<a href="#-features">Features</a> •
<a href="#-install">Install</a> •
<a href="#-first-scan">First scan</a> •
<a href="#-build-from-source">Build</a> •
<a href="docs/DEVELOPMENT.md">Developer docs</a> •
<a href="#-support">Support</a>

<br>

<a href="https://ko-fi.com/laborantsoftworks"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a>
</div>

---

## ✨ Features

<table>
<tr>
<td width="50%" valign="top">

### 🎞️ Scanning
- **Plug in and scan.** PakonX finds the scanner when you switch it on and loads its firmware by itself.
- **Base 4, 8 and 16**, with infrared for **dust removal (ICE)**.
- **Orders and batches**: roll numbers, one folder per order, like PSI.
- **Advance < / >** to free stuck film at full speed.

</td>
<td width="50%" valign="top">

### 🧪 Developing
- **Two looks**: the original **Pakon look** (the OEM's own engine, Windows) or **PakonX's look** (open and adjustable).
- **Exposure, colour and contrast** with exact number fields and − / + steps.
- **Film presets by DX code**: grade one roll of Portra and every Portra roll opens with it.
- **Frame formats**: full, half frame and panoramic, with framing correction.

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 💾 Saving
- **JPEG or 16-bit TIFF**, named by roll and DX frame number (`1111_19A.jpg`) or your own scheme.
- **Open in Negative Lab Pro**: flat linear negatives straight into Lightroom Classic.
- Contact sheet, light table with 100 % zoom, rotation, multi-select.

</td>
<td width="50%" valign="top">

### 🔓 Free software
- **GPL-3.0.** No licence keys, no trial, no account, no telemetry.
- Built from a careful, evidence-cited reverse engineering of the scanner ([docs](docs/DEVELOPMENT.md)).
- Updates come from GitHub releases, signed.

</td>
</tr>
</table>

## 📦 Install

Download the file for your system from the **[latest release](https://github.com/amstrd-cpc/PakonX/releases/latest)**.

<details open>
<summary><b>Windows 10 / 11 (x64 and ARM64)</b></summary>

Run `PakonX_1.0.0_x64-setup.exe`. The installer also installs the USB driver (Microsoft's WinUSB) for the scanner.

**Windows on ARM** (Snapdragon): an ARM64 installer is built with [`tools/build-windows-arm64.ps1`](tools/build-windows-arm64.ps1) and added to releases when available; meanwhile the x64 installer runs under Windows' emulation.

</details>

<details>
<summary><b>macOS 13.3+ (Apple Silicon and Intel)</b></summary>

Open the `.dmg` and drag PakonX to Applications. The app is not notarized (that needs a paid Apple account), so the first time **right-click PakonX → Open**, or run:

```sh
xattr -dr com.apple.quarantine /Applications/PakonX.app
```

</details>

<details>
<summary><b>Linux (x64)</b></summary>

**Debian / Ubuntu:** `sudo apt install ./PakonX_1.0.0_amd64.deb` (installs the USB permission rule too).

**AppImage:** make it executable and run it. Once, give your user access to the scanner:

```sh
sudo curl -fsSL https://raw.githubusercontent.com/amstrd-cpc/PakonX/main/apps/pakonx/src-tauri/linux/60-pakon.rules -o /etc/udev/rules.d/60-pakon.rules
sudo udevadm control --reload && sudo udevadm trigger
```

</details>

> [!NOTE]
> The macOS and Linux versions talk to the scanner through libusb and are new in 1.0.0: the firmware loader there has not yet been run on a real scanner. Windows is the tested platform. Reports are very welcome in [Issues](https://github.com/amstrd-cpc/PakonX/issues).

## 🚀 First scan

1. Connect the scanner by USB and switch it on. PakonX shows **Ready** after a few seconds.
2. **New order**: type the roll number from the sticker.
3. Choose the resolution and **Scan**, then feed the film.
4. The roll opens on the **Roll** tab. Adjust the look, then **Save roll**.

Something stuck? Use **Advance < / >** in the Scanner panel. Everything PakonX does is written to the **Log** at the bottom.

## 🛠️ Build from source

```sh
# pakon-cli (C++20, CMake; on Linux/macOS install libusb-1.0 first)
cmake -S . -B build && cmake --build build --config Release
ctest --test-dir build -C Release

# the app (Tauri 2: Node 18+, Rust stable)
cd apps/pakonx && npm install
npm run dev
```

Platform builds: Windows x64 `npm run build`, Windows ARM64 [`tools/build-windows-arm64.ps1`](tools/build-windows-arm64.ps1), Linux and macOS through [GitHub Actions](.github/workflows/desktop.yml). Details in [apps/pakonx/README.md](apps/pakonx/README.md) and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

> [!IMPORTANT]
> Hardware safety is binding for contributors: no lamp, motor or scan path runs without the operator's explicit action, EEPROM writes are never sent, and the boot path only writes volatile FX2 RAM. See [CLAUDE.md](CLAUDE.md) and [docs/HARDWARE_RUNBOOK.md](docs/HARDWARE_RUNBOOK.md).

## ☕ Support

PakonX is free and stays free. If it saved your scanner from the landfill, you can say thanks:

<a href="https://ko-fi.com/laborantsoftworks"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a>
<a href="https://github.com/sponsors/amstrd-cpc"><img src="https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors" height="36"></a>

## 📜 License

PakonX is free software under the [GNU General Public License v3.0](LICENSE).<br>
Kodak and Pakon are trademarks of their owners; this project is not affiliated with them. The scanner firmware and the OEM look library bundled in the Windows installer remain the property of their owners.

<div align="center">

Made by <a href="https://github.com/amstrd-cpc">Erik Muradian</a> · <a href="https://www.instagram.com/amstrd_cpc">@amstrd_cpc</a>

</div>
