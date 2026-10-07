# End-to-end loopback test: fake iPhone -> AirGlass (--loopback), with timed window captures.
param([string[]]$Media = @('testmedia\portrait.mp4', 'testmedia\landscape.mp4'),
      [double[]]$At = @(0.5, 1.5, 3.0, 6.8, 8.0, 12.5),
      [string]$Extra = '',
      [string]$Shots = (Join-Path $env:TEMP 'airglass-shots'))
$build = Join-Path (Split-Path $PSScriptRoot) 'build'
Set-Location $build
New-Item -ItemType Directory -Force $Shots | Out-Null
Get-Process AirGlass -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$build\*" } | Stop-Process -Confirm:$false
Start-Sleep -Milliseconds 300
$app = Start-Process -FilePath .\AirGlass.exe -ArgumentList '--loopback', '--debug' -PassThru
Start-Sleep -Milliseconds 1500
$args2 = @('stream') + $Media + @('--port', '7010')
if ($Extra) { $args2 += $Extra.Split(' ') }
$test = Start-Process -FilePath .\airglass_test.exe -ArgumentList $args2 -RedirectStandardOutput "$Shots\test_out.txt" -RedirectStandardError "$Shots\test_err.txt" -PassThru -NoNewWindow
& (Join-Path $PSScriptRoot 'capture.ps1') -OutPrefix "$Shots\e2e" -At $At
$test.WaitForExit(30000) | Out-Null
"--- test output"
Get-Content "$Shots\test_out.txt" | Select-Object -Last 6
Start-Sleep -Milliseconds 800
Stop-Process -Id $app.Id -Confirm:$false -ErrorAction SilentlyContinue
"--- receiver log (info+)"
Get-Content "$env:LOCALAPPDATA\AirGlass\airglass-loopback.log" | Where-Object { $_ -notmatch '\[DBG\]' } | Select-Object -Last 40
