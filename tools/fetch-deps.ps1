# Downloads the build dependencies that are not kept in git:
#   .toolchain\llvm-mingw-<ver>-ucrt-x86_64   (clang/lld for Windows, mstorsjo/llvm-mingw)
#   third_party\ffmpeg-n8.1-...-lgpl-shared-8.1 (FFmpeg LGPL shared build, BtbN/FFmpeg-Builds)
# The Windows 10/11 SDK (for fxc.exe) and Python 3 must already be installed.
#   powershell -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
param(
    [string]$LlvmMingw = '20260922',
    [string]$FfmpegBranch = '8.1'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is very slow with the progress bar
$root = Split-Path $PSScriptRoot
$dl = Join-Path $root 'third_party\dl'
New-Item -ItemType Directory -Force $dl, (Join-Path $root '.toolchain') | Out-Null

function Get-Dep([string]$Url, [string]$Zip, [string]$Dest, [string]$Folder) {
    if (Test-Path (Join-Path $Dest $Folder)) { Write-Host "ok        $Folder"; return }
    $zipPath = Join-Path $dl $Zip
    if (-not (Test-Path $zipPath)) {
        Write-Host "download  $Url"
        Invoke-WebRequest $Url -OutFile $zipPath -UseBasicParsing
    }
    Write-Host "unpack    $Folder"
    Expand-Archive $zipPath -DestinationPath $Dest -Force
}

$llvm = "llvm-mingw-$LlvmMingw-ucrt-x86_64"
Get-Dep "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmMingw/$llvm.zip" 'llvm-mingw.zip' `
    (Join-Path $root '.toolchain') $llvm

$ff = "ffmpeg-n$FfmpegBranch-latest-win64-lgpl-shared-$FfmpegBranch"
Get-Dep "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$ff.zip" 'ffmpeg.zip' `
    (Join-Path $root 'third_party') $ff

Write-Host 'Dependencies ready. Next: python build.py'
