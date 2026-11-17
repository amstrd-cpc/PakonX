# PakonX installer, elevated (NSIS post-install hook): binds the scanner to
# Microsoft's in-box WinUSB driver with driver\PakonWinUSB.inf.
#
# Windows installs a driver package only with a trusted, signed catalog.
# Like Zadig/libwdi, this PC gets its own signing certificate: created
# here, trusted (LocalMachine Root + TrustedPublisher), used once to sign
# pakonwinusb.cat, and its private key deleted at once, so nothing else
# can ever be signed with it. No test mode, no network. The package holds
# no driver binary: winusb.sys is Microsoft's.
param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$Subject = 'CN=PakonX scanner driver (this PC)'
$log = Join-Path $Dir 'install.log'
# NSIS is a 32-bit program, so on 64-bit Windows this runs in 32-bit
# PowerShell: System32 is redirected to SysWOW64, which has no pnputil.exe.
# Sysnative is the real System32 (it exists only for 32-bit processes).
$pnputil = Join-Path $env:windir 'sysnative\pnputil.exe'
if (-not (Test-Path $pnputil)) { $pnputil = Join-Path $env:windir 'System32\pnputil.exe' }
function Log($m) { "$(Get-Date -Format s)  $m" | Tee-Object -FilePath $log -Append }

try {
    Log "driver folder: $Dir"
    # A reinstall replaces the earlier certificate.
    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $Subject } |
            ForEach-Object { Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue; Log "removed old certificate from $store" }
    }
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(30)
    $cer = Join-Path $env:TEMP ('pakonx-driver-' + [guid]::NewGuid().ToString('N') + '.cer')
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Remove-Item $cer
    Log "certificate $($cert.Thumbprint) trusted"

    $sig = Set-AuthenticodeSignature -FilePath (Join-Path $Dir 'pakonwinusb.cat') -Certificate $cert -HashAlgorithm SHA256
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
    Log "catalog signed: $($sig.Status); private key deleted"
    if ($sig.Status -ne 'Valid') { throw "catalog signature: $($sig.StatusMessage)" }

    $out = & $pnputil /add-driver (Join-Path $Dir 'PakonWinUSB.inf') /install 2>&1 | Out-String
    $code = $LASTEXITCODE
    Log "pnputil exit $code`n$out"
    # 259: added, but no scanner plugged in right now (it binds when plugged).
    # 3010: added, a restart finishes it.
    if ($code -notin 0, 259, 3010) { throw "pnputil /add-driver failed ($code)" }
    exit 0
} catch {
    Log "FAILED: $_"
    exit 1
}
