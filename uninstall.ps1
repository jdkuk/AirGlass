# Removes AirGlass for the current user (files, shortcut, autostart, firewall rule).
$dst = Join-Path $env:LOCALAPPDATA 'Programs\AirGlass'
$exe = Join-Path $dst 'AirGlass.exe'
if (Test-Path $exe) {
    $q = Start-Process -FilePath $exe -ArgumentList '--quit' -PassThru
    $q.WaitForExit(6000) | Out-Null
}
Get-Process AirGlass -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'AirGlass' -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'AirGlass.lnk') -ErrorAction SilentlyContinue
Start-Process -FilePath powershell.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList @('-NoProfile', '-Command',
    "Get-NetFirewallRule -DisplayName 'AirGlass (AirPlay receiver)' -ErrorAction SilentlyContinue | Remove-NetFirewallRule")
Start-Sleep -Milliseconds 500
Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'AirGlass removed. Settings and logs remain in %LOCALAPPDATA%\AirGlass.'
