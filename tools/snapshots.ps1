# Headless visual test: AirGlass renders hidden and saves frames; the fake iPhone streams.
param([string]$Times = '0.12,0.45,1.6,3.25,4.6,5.35,6.6,7.0,8.8',
      [string]$Controls = '2', [string]$Fullscreen = '3.0,5.0',
      [string[]]$Media = @('testmedia\portrait.mp4', 'testmedia\landscape.mp4'),
      [string]$Out = (Join-Path $env:TEMP 'airglass-snaps'))
$build = Join-Path (Split-Path $PSScriptRoot) 'build'
Set-Location $build
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter 'snap_*' | ForEach-Object { $_.Delete() }
Get-Process AirGlass -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$build\AirGlass.exe" } | Stop-Process -Confirm:$false
$env:AIRGLASS_DEBUG_SNAPSHOT = $Out
$env:AIRGLASS_DEBUG_SNAPSHOT_TIMES = $Times
if ($Controls) { $env:AIRGLASS_DEBUG_CONTROLS = $Controls }
if ($Fullscreen) { $env:AIRGLASS_DEBUG_FULLSCREEN = $Fullscreen }
$app = Start-Process -FilePath .\AirGlass.exe -ArgumentList '--loopback', '--debug' -PassThru
$env:AIRGLASS_DEBUG_SNAPSHOT = $null; $env:AIRGLASS_DEBUG_SNAPSHOT_TIMES = $null
$env:AIRGLASS_DEBUG_CONTROLS = $null; $env:AIRGLASS_DEBUG_FULLSCREEN = $null
Start-Sleep -Milliseconds 1200
$args2 = @('stream') + $Media + @('--port', '7010')
$test = Start-Process -FilePath .\airglass_test.exe -ArgumentList $args2 -RedirectStandardOutput "$Out\test_out.txt" -RedirectStandardError "$Out\test_err.txt" -PassThru -NoNewWindow
$test.WaitForExit(40000) | Out-Null
Start-Sleep -Milliseconds 600
Stop-Process -Id $app.Id -Confirm:$false -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing
foreach ($f in Get-ChildItem $Out -Filter 'snap_*.bmp') {
  $img = [System.Drawing.Image]::FromFile($f.FullName)
  $png = [System.IO.Path]::ChangeExtension($f.FullName, '.png')
  $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
  "{0}  {1}x{2}" -f [System.IO.Path]::GetFileName($png), $img.Width, $img.Height
}
Get-Content "$env:LOCALAPPDATA\AirGlass\airglass-loopback.log" | Select-String 'snapshot|WRN|ERR' | ForEach-Object { $_.Line }
Get-Content "$Out\test_out.txt" | Select-Object -Last 2
