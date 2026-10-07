# Builds a release into dist\: the installer (Inno Setup 6) and the matching source zip
# (the GPL requires the source to go with every copy sold).
#   powershell -ExecutionPolicy Bypass -File package.ps1 [-Version 1.0.0] [-SkipBuild]
param([string]$Version = '1.0.0', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (Select-String -Path src\license.h -Pattern 'kStoreId = 0;' -Quiet) {
    throw 'Set license::kStoreId / kProductId / kBuyUrl in src\license.h to the Lemon Squeezy product first.'
}
if (-not $SkipBuild) {
    python build.py
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
$ffmpeg = (Get-ChildItem third_party -Directory -Filter 'ffmpeg-*' | Select-Object -First 1).FullName

New-Item -ItemType Directory -Force dist | Out-Null
& $iscc /Q "/DAppVersion=$Version" "/DFfmpegDir=$ffmpeg" installer.iss
if ($LASTEXITCODE -ne 0) { throw 'installer build failed' }

# Source zip: exactly what git tracks (no toolchain, downloads or build output).
$zip = "dist\AirGlass-$Version-source.zip"
git archive --format=zip --prefix="AirGlass-$Version/" -o $zip HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed (commit first)' }
if (git status --porcelain) { Write-Warning 'Uncommitted changes are NOT in the source zip - commit before releasing.' }

Get-ChildItem dist | Where-Object Name -like "*$Version*" |
    ForEach-Object { '{0,-40} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) }
