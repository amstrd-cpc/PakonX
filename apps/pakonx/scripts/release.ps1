# Builds the signed PakonX installer and, with -Publish, makes it a GitHub
# release of amstrd-cpc/PakonX with the latest.json the in-app updater reads
# (tauri.conf.json: releases/latest/download/latest.json). The signing key
# stays outside the repository: -KeyDir (pakonx.key, password.txt).
#
#   cd apps\pakonx
#   .\scripts\release.ps1 -KeyDir C:\keys\pakonx                           # build only
#   .\scripts\release.ps1 -KeyDir C:\keys\pakonx -Publish -Notes "What changed"
param([Parameter(Mandatory)][string]$KeyDir, [string]$Notes = "", [switch]$Publish)
$ErrorActionPreference = 'Stop'

$env:TAURI_SIGNING_PRIVATE_KEY = Get-Content -Raw (Join-Path $KeyDir 'pakonx.key')
$env:TAURI_SIGNING_PRIVATE_KEY_PASSWORD = (Get-Content -Raw (Join-Path $KeyDir 'password.txt')).Trim()
$version = (Get-Content -Raw (Join-Path $PSScriptRoot '..\package.json') | ConvertFrom-Json).version

Push-Location (Join-Path $PSScriptRoot '..')
try { npm run build; if ($LASTEXITCODE) { throw "npm run build failed" } } finally { Pop-Location }
$exe = Join-Path $PSScriptRoot "..\src-tauri\target\release\bundle\nsis\PakonX_${version}_x64-setup.exe" | Resolve-Path
if (-not (Test-Path "$exe.sig")) { throw "$exe.sig is missing: the build was not signed" }
"Built and signed: $exe"
if (-not $Publish) { "Not published. Rerun with -Publish to release PakonX $version on GitHub."; return }

$file = Split-Path -Leaf $exe
$manifest = [ordered]@{
  version   = $version
  notes     = $Notes
  pub_date  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  platforms = [ordered]@{
    'windows-x86_64' = [ordered]@{
      signature = (Get-Content -Raw "$exe.sig").Trim()
      url       = "https://github.com/amstrd-cpc/PakonX/releases/download/v$version/$file"
    }
  }
}
$json = Join-Path ([IO.Path]::GetTempPath()) 'latest.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Path $json -Encoding utf8
gh release create "v$version" "$exe" "$exe.sig" $json --repo amstrd-cpc/PakonX --title "PakonX $version" --notes $Notes
if ($LASTEXITCODE) { throw "gh release create failed" }
"Released PakonX ${version}: https://github.com/amstrd-cpc/PakonX/releases/tag/v$version"
