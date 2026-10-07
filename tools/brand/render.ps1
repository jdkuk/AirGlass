# Renders the README/brand images in docs\images with headless Microsoft Edge (no window shown).
#   powershell -ExecutionPolicy Bypass -File tools\brand\render.ps1
# docs\images\window-*.png are real AirGlass frames: run tools\snapshots.ps1 with
# AIRGLASS_DEBUG_SNAPSHOT_RAW=1 and convert the .bgra dumps (see docs/development.md).
$ErrorActionPreference = 'Stop'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
$images = Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'docs\images'
$profileDir = Join-Path $env:TEMP 'airglass-brand-edge'
$jobs = @(
    @{ Page = 'hero.html'; Out = 'hero.png'; Size = '1600,900' },
    @{ Page = 'banner.html'; Out = 'banner.png'; Size = '1280,640' }
)
foreach ($j in $jobs) {
    $url = 'file:///' + (Join-Path $PSScriptRoot $j.Page).Replace('\', '/')
    $out = Join-Path $images $j.Out
    Start-Process -Wait -FilePath $edge -ArgumentList '--headless=new', '--disable-gpu', '--hide-scrollbars',
        "--user-data-dir=`"$profileDir`"", "--window-size=$($j.Size)", '--virtual-time-budget=3000',
        "--screenshot=`"$out`"", $url
    Write-Host "rendered $($j.Out)"
}
