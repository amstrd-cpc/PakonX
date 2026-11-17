# PakonX uninstaller, elevated (NSIS pre-uninstall hook): removes the
# WinUSB driver package and the certificate install-driver.ps1 trusted.
$ErrorActionPreference = 'Continue'
$Subject = 'CN=PakonX scanner driver (this PC)'
# 32-bit PowerShell on 64-bit Windows (NSIS): pnputil.exe is only in the
# real System32, reached through Sysnative (see install-driver.ps1).
$pnputil = Join-Path $env:windir 'sysnative\pnputil.exe'
if (-not (Test-Path $pnputil)) { $pnputil = Join-Path $env:windir 'System32\pnputil.exe' }
# The package's published copies (oemNN.inf), found by the interface GUID in
# PakonWinUSB.inf rather than by pnputil's text, which Windows translates.
$guid = '0e9e6f29-e70a-4582-8d02-bde3ad701252'
Get-ChildItem (Join-Path $env:windir 'INF\oem*.inf') -ErrorAction SilentlyContinue |
    Where-Object { Select-String -Path $_.FullName -Pattern $guid -SimpleMatch -Quiet } |
    ForEach-Object { & $pnputil /delete-driver $_.Name /uninstall /force | Out-Null }
foreach ($store in 'Root', 'TrustedPublisher', 'My') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $Subject } |
        ForEach-Object { Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue }
}
exit 0
