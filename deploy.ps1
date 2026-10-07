# Installs (or updates) AirGlass for the current user and starts it.
#   - %LOCALAPPDATA%\Programs\AirGlass   (no admin needed)
#   - Start menu shortcut, start with Windows
#   - one inbound Windows Firewall rule for AirGlass.exe (asks for elevation once)
param([switch]$SkipFirewall, [switch]$NoLaunch)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$dst = Join-Path $env:LOCALAPPDATA 'Programs\AirGlass'
$exe = Join-Path $dst 'AirGlass.exe'
$ruleName = 'AirGlass (AirPlay receiver)'

# 1. Stop a running copy (graceful first so it sends its mDNS goodbye).
if (Test-Path $exe) {
    $q = Start-Process -FilePath $exe -ArgumentList '--quit' -PassThru
    $q.WaitForExit(6000) | Out-Null
}
Get-Process AirGlass -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
Start-Sleep -Milliseconds 300

# 2. Files.
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $src 'build\AirGlass.exe') $dst -Force
foreach ($d in 'avcodec-62.dll', 'avutil-60.dll', 'swresample-6.dll') { Copy-Item (Join-Path $src "build\$d") $dst -Force }
Copy-Item (Join-Path $src 'README.md') $dst -Force
Copy-Item (Join-Path $src 'uninstall.ps1') $dst -Force
Write-Host "Installed to $dst"

# 3. Firewall: allow inbound for this program on all profiles (this PC's network is 'Public').
if (-not $SkipFirewall) {
    $existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    $ok = $false
    if ($existing) {
        $app = $existing | Get-NetFirewallApplicationFilter
        $ok = ($app.Program -eq $exe) -and ($existing.Enabled -eq 'True') -and ($existing.Action -eq 'Allow')
    }
    if (-not $ok) {
        Write-Host 'Requesting permission (UAC) to add the firewall rule...'
        $cmd = if ($existing) {
            "Set-NetFirewallRule -DisplayName '$ruleName' -Enabled True -Action Allow -Profile Any; Get-NetFirewallRule -DisplayName '$ruleName' | Set-NetFirewallApplicationFilter -Program '$exe'"
        } else {
            "New-NetFirewallRule -DisplayName '$ruleName' -Description 'Lets iPhone, iPad and Mac discover AirGlass (mDNS) and stream Screen Mirroring to it.' -Direction Inbound -Action Allow -Program '$exe' -Profile Any | Out-Null"
        }
        try {
            $p = Start-Process -FilePath powershell.exe -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd) -PassThru -WindowStyle Hidden
            $p.WaitForExit(300000) | Out-Null
        } catch {
            Write-Warning "Firewall rule not added ($($_.Exception.Message)). Windows will ask on first use instead - tick BOTH Private and Public networks."
        }
    }
    if (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue) { Write-Host "Firewall rule '$ruleName' is in place" }
}

# 4. Start menu shortcut.
$lnkPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'AirGlass.lnk'
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($lnkPath)
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $dst
$lnk.IconLocation = "$exe,0"
$lnk.Description = 'AirPlay screen mirroring receiver'
$lnk.Save()
Write-Host "Start menu shortcut: $lnkPath"

# 5. Start with Windows (toggle later from the tray menu).
Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'AirGlass' -Value "`"$exe`" --background"
Write-Host 'Autostart enabled'

# 6. Launch through Explorer so it runs as a normal desktop app.
if (-not $NoLaunch) {
    Start-Process -FilePath explorer.exe -ArgumentList "`"$exe`""
    Write-Host 'AirGlass started (look for the tray icon)'
}
