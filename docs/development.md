# Development & testing

AirGlass can be tested end to end on one PC **without an iPhone and without any window appearing**. `airglass_test.exe`
is a loopback AirPlay *sender*, and `AirGlass.exe --loopback` is a receiver that only listens on 127.0.0.1. The headless
snapshot mode renders the glass window off-screen and saves frames as images.

## Self-tests

```powershell
.\build\airglass_test.exe resampler                       # resampler quality (SNR, gain flatness) -> PASS resampler
python tools\bplist_check.py $env:TEMP\bp build\airglass_test.exe   # bplist vs Python plistlib -> PASS
Start-Process -Wait build\AirGlass.exe --selftest          # crypto known-answer tests (exit code 0 = pass)
```

The crypto self-test (RFC 7748 X25519, RFC 8032 Ed25519, NIST AES, SHA-512) also runs at every start-up.

## Loopback end-to-end

```powershell
# 1. Receiver: loopback only (port 7010, pipe AirGlass.Control.Loopback, log airglass-loopback.log)
$env:AIRGLASS_DEBUG_SNAPSHOT = "$env:TEMP\snaps"     # keep the window hidden and save frames instead
Start-Process build\AirGlass.exe '--loopback','--debug'

# 2. Sender: full handshake (pair-verify, FairPlay, SETUP), encrypted H.264 + AAC-ELD, then teardown
.\build\airglass_test.exe stream testmedia\portrait.mp4 testmedia\landscape.mp4 --port 7010   # -> PASS stream

# 3. Music: iOS-style ALAC + DMAP metadata + artwork + progress + DACP remote
.\build\airglass_test.exe music testmedia\music_alac.m4a --art testmedia\art.jpg --port 7010  # -> PASS music
```

`tools\snapshots.ps1` and `tools\e2e.ps1` wrap this with timed captures. Test media can be generated with the bundled
FFmpeg (`libopenh264` and `alac` are in the LGPL build):

```powershell
$ff = (Resolve-Path third_party\ffmpeg-*\bin\ffmpeg.exe)
& $ff -f lavfi -i testsrc2=s=886x1920:r=60:d=8 -c:v libopenh264 -b:v 8M -pix_fmt yuv420p testmedia\portrait.mp4
& $ff -f lavfi -i "sine=f=440:d=8" -ac 2 -ar 44100 -c:a alac testmedia\music_alac.m4a
```

> [!IMPORTANT]
> Never run `AirGlass.exe --quit` during a loopback test: it targets the *installed* instance, not the loopback one.

## Command-line switches

| Switch | Effect |
|---|---|
| `--background` | start silently in the tray (used by autostart) |
| `--loopback` | 127.0.0.1 only, port 7010, separate single-instance mutex, pipe and log; no mDNS |
| `--debug` | debug-level log |
| `--selftest` | run the crypto self-test and exit (0 = pass) |
| `--quit` | ask the running (installed) instance to quit gracefully |
| `--write-icon <path>` | write the generated app icon as `.ico` |

## Debug environment variables

| Variable | Effect |
|---|---|
| `AIRGLASS_DEBUG_SNAPSHOT=<dir>` | render the window off-screen and save `snap_NN.bmp` frames there |
| `AIRGLASS_DEBUG_SNAPSHOT_TIMES=0.5,2,4` | when to take snapshots (seconds after the session starts) |
| `AIRGLASS_DEBUG_SNAPSHOT_RAW=1` | also save premultiplied BGRA dumps (`snap_NN_WxH.bgra`) with real transparency |
| `AIRGLASS_DEBUG_CONTROLS=<0-3>` | show the control capsule with that button hovered |
| `AIRGLASS_DEBUG_FULLSCREEN=in,out` | enter / leave full screen at those times |
| `AIRGLASS_DEBUG_FREE=1` | loopback runs use the free edition (loopback defaults to Pro) |
| `AIRGLASS_DEBUG_AUDIODUMP=<file>` | music sessions: write the decoded PCM to that file, for bit-exactness checks |
| `AIRGLASS_DEBUG_DACP_PORT=<port>` | talk to a fake DACP remote on localhost |

## Config file

`%LOCALAPPDATA%\AirGlass\config.ini` (created on first run):

```ini
[receiver]
name=Living Room PC
port=7000
deviceid=…
key=…
[video]
width=0
height=0
refresh=0
maxfps=0
[app]
debuglog=0
[license]
key=…
instance=…
```

* `name` is what senders show. `deviceid` and `[receiver] key` are the receiver's persistent identity, so don't share them.
* `[video]` values of `0` mean automatic: the monitor's shape, capped at 1920×1080, refresh capped at 120 Hz.
* `[license]` holds the AirGlass Pro key and its activation id.
* Don't put comments on the same line as a value: Windows' INI reader keeps them as part of the value.

## README / marketing images

`docs/images/window-*.png` are real frames: run `tools\snapshots.ps1` with `AIRGLASS_DEBUG_SNAPSHOT_RAW=1`, convert the
`.bgra` dumps to PNG (premultiplied BGRA = GDI+ `Format32bppPArgb`), then `tools\brand\render.ps1` composes
`hero.png` and `banner.png` with headless Edge. `banner.png` (1280×640) is also the GitHub social preview.

## Code style

* C++20, 4-space indent, 120 columns, `PascalCase` functions and types, `camelCase_` members.
* Comments explain *why*. Keep them short.
* No new warnings (`-Wall -Wextra`).
* UI thread vs render thread vs network threads are documented in [Architecture](architecture.md). Respect the locks.
