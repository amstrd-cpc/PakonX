# PakonX for Windows on ARM (Snapdragon / ARM64 laptops), built on this
# x64 PC. One-time setup:
#   - Visual Studio Installer > Modify > Individual components:
#     "MSVC v14x - VS 20xx C++ ARM64/ARM64EC build tools (Latest)"
#   - rustup target add aarch64-pc-windows-msvc
# Then, from the repository root:
#   powershell -ExecutionPolicy Bypass -File .\tools\build-windows-arm64.ps1
# The installer lands in apps\pakonx\src-tauri\target\aarch64-pc-windows-msvc\release\bundle\nsis.
# It bundles the same OEM look (oem_host.exe is 32-bit x86: Windows on
# ARM runs it emulated) and the driver package (PakonWinUSB.inf has an
# NTarm64 section; its catalog covers 10_X64 and 10_RS3_ARM64).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
cmake -S . -B build-arm64 -A ARM64 -DPAKON_BUILD_TESTS=OFF
cmake --build build-arm64 --config Release --target pakon-cli
Set-Location apps\pakonx
npx tauri build --target aarch64-pc-windows-msvc --config src-tauri\tauri.windows-arm64.conf.json
