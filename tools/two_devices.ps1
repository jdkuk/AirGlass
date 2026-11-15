# Loopback test of two devices mirroring at once (run from the repo root after build.py and the test media).
#   Pro (default): two senders mirror at the same time, each to its own window and decoder; then a third sender
#                  takes over the oldest of two.
#   -Free:         the free edition keeps one session: a second sender takes over the first.
# The receiver renders hidden (AIRGLASS_DEBUG_SNAPSHOT) and writes snap_00.bmp (window 1) / snap_w2_00.bmp
# (window 2). Exit code 0 = pass.
param([switch]$Free,
      [string]$Snaps = (Join-Path $env:TEMP 'airglass-two'))
$ErrorActionPreference = 'Stop'
$log = "$env:LOCALAPPDATA/AirGlass/airglass-loopback.log"
$portrait = 'build/testmedia/portrait.mp4'
$landscape = 'build/testmedia/landscape.mp4'
$out = Join-Path $Snaps 'senders'
New-Item -ItemType Directory -Force $Snaps, $out | Out-Null
Remove-Item "$Snaps/snap_*" -ErrorAction SilentlyContinue

function Send([string]$name, [string[]]$media) {
    $p = Start-Process build/airglass_test.exe -ArgumentList (@('stream') + $media + @('--port', '7010', '--name', $name)) `
        -RedirectStandardOutput "$out/$name.txt" -NoNewWindow -PassThru
    $null = $p.Handle  # keeps ExitCode readable once it has ended
    $p
}
function Finish($p, [string]$name) {
    if (-not $p.WaitForExit(90000)) { $p.Kill() }
    Get-Content "$out/$name.txt" | Select-Object -Last 2 | ForEach-Object { Write-Host "  [$name] $_" }
    Write-Host "  [$name] exit code $($p.ExitCode)"
    return $p.ExitCode
}

# GET /status (always on in loopback mode): "mirror" is the most recent session, "sessions" lists every live one.
function Status {
    try { Invoke-RestMethod 'http://127.0.0.1:7010/status?wait=0' -TimeoutSec 5 } catch { Write-Host "  status: $_"; $null }
}
function Live($st) { @($st.sessions | Where-Object { $_ }) }  # [] and null both count 0
function Devices($st) { (Live $st | ForEach-Object { $_.device }) -join ',' }

$ok = $true
function Expect([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "PASS $what" } else { Write-Host "FAIL $what"; $script:ok = $false }
}

$env:AIRGLASS_DEBUG_SNAPSHOT = $Snaps
$env:AIRGLASS_DEBUG_SNAPSHOT_TIMES = '2.0'
if ($Free) { $env:AIRGLASS_DEBUG_FREE = '1' } else { Remove-Item Env:AIRGLASS_DEBUG_FREE -ErrorAction SilentlyContinue }
$app = Start-Process build/AirGlass.exe -ArgumentList '--loopback', '--debug' -PassThru
Start-Sleep 3

if ($Free) {
    # One session at a time: the iPad takes over from the iPhone, whose sender sees the session end (exit 3).
    $a = Send 'iPhone-F1' @($portrait, $landscape)
    Start-Sleep 2
    $b = Send 'iPad-F2' @($landscape)
    Start-Sleep 2
    $st = Status
    Write-Host "  status: mirror=$($st.mirror.device) sessions=$(Devices $st)"
    Expect ($st.mirror.device -eq 'iPad-F2' -and (Live $st).Count -eq 1) 'status: one session, the new device'
    Expect ((Finish $b 'iPad-F2') -eq 0) 'the second device mirrors to the end'
    Expect ((Finish $a 'iPhone-F1') -eq 3) 'the first device was taken over'
} else {
    # Two at once: an iPhone, and 2 s later an iPad; both run to the end.
    $a = Send 'iPhone-A' @($portrait, $landscape)
    Start-Sleep 2
    $b = Send 'iPad-B' @($landscape)
    Start-Sleep 2
    $st = Status
    Write-Host "  status: mirror=$($st.mirror.device) sessions=$(Devices $st)"
    $recent = @(Live $st | Where-Object { $_.recent })
    Expect ($st.mirror.device -eq 'iPad-B' -and $st.mirror.width -ge 0 -and $st.mirror.mode) 'status: mirror is the most recent session'
    Expect ((Live $st).Count -eq 2 -and (Devices $st) -like '*iPhone-A*' -and (Devices $st) -like '*iPad-B*') 'status: sessions lists both'
    Expect ($recent.Count -eq 1 -and $recent.device -eq 'iPad-B') 'status: one session is the recent one'
    Expect ((Finish $b 'iPad-B') -eq 0) 'the second device mirrors to the end next to the first'
    Expect ((Finish $a 'iPhone-A') -eq 0) 'the first device keeps mirroring while the second does'
    # A third device takes over the oldest of two.
    $a = Send 'iPhone-A2' @($portrait, $landscape)
    Start-Sleep 2
    $b = Send 'iPad-B2' @($portrait, $landscape)
    Start-Sleep 2
    $c = Send 'iPhone-C3' @($landscape)
    Expect ((Finish $c 'iPhone-C3') -eq 0) 'the third device mirrors to the end'
    Expect ((Finish $b 'iPad-B2') -eq 0) 'the newer of the two keeps mirroring'
    Expect ((Finish $a 'iPhone-A2') -eq 3) 'the oldest session was taken over'
}

Start-Sleep 1
$st = Status
Expect ($st -and $null -eq $st.mirror -and (Live $st).Count -eq 0) 'status: no session once all have ended'
Stop-Process -Id $app.Id -ErrorAction SilentlyContinue
Start-Sleep 1
$text = Get-Content $log -Raw
$frames = ([regex]::Matches($text, 'video: first frame')).Count
if ($Free) {
    Expect ($text -like '*edition: free*') 'free edition'
    Expect ($text -like '*takes over mirroring from*') 'server: takeover'
    Expect ($text -like '*takes over window 1*') 'app: the window was taken over'
    Expect ($text -notlike '*mirrors to window 2*') 'no second window'
    Expect ($text -like '*upgrade dialog suppressed*') 'the Pro dialog is offered (suppressed in loopback)'
    Expect ($frames -ge 2) "two decoders, one after the other ($frames)"
    Copy-Item $log "$env:LOCALAPPDATA/AirGlass/airglass-loopback-free.log"
} else {
    Expect ($text -like '*second mirroring window ready*') 'the second window was made'
    Expect ($text -like '*mirrors to window 2 (2 of 2 at once)*') 'server: two sessions at once'
    Expect ($text -like '*; 2 mirroring at once*') 'app: two windows live at once'
    Expect ($text -like '*takes over the oldest mirroring session*') 'server: a third takes over the oldest'
    Expect ($frames -ge 5) "a decoder per session, five sessions ($frames)"
    Expect (Test-Path "$Snaps/snap_00.bmp") 'window 1 rendered (snap_00.bmp)'
    Expect (Test-Path "$Snaps/snap_w2_00.bmp") 'window 2 rendered (snap_w2_00.bmp)'
    Copy-Item $log "$env:LOCALAPPDATA/AirGlass/airglass-loopback-two.log"
}
$text -split "`n" | Select-String 'sessions:|takes over|mirrors to window|the sound|edition:|first frame' | Select-Object -Last 30
if (-not $ok) { exit 1 }
'PASS two devices' + $(if ($Free) { ' (free edition: takeover)' } else { ' (Pro)' })
