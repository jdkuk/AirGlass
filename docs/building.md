# Building AirGlass

AirGlass builds with **clang from [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)** (UCRT, x86-64) against the
**FFmpeg LGPL shared build** from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds). Shaders are compiled with
`fxc` from the Windows SDK. There's no Visual Studio project and no CMake, just one Python script.

## Requirements

| | Version | Notes |
|---|---|---|
| Windows | 10 or 11, x64 | |
| Python | 3.9+ | runs `build.py` |
| Windows 10/11 SDK | any 10.0.x | only `fxc.exe` is used; the newest installed SDK is picked automatically |
| llvm-mingw | 20260922 (UCRT) | downloaded by `tools\fetch-deps.ps1` into `.toolchain\` |
| FFmpeg | n8.1, LGPL shared | downloaded by `tools\fetch-deps.ps1` into `third_party\` |
| Inno Setup | 6 | only for the installer (`package.ps1`) |

```powershell
winget install Python.Python.3.12 Microsoft.WindowsSDK.10.0.22621 JRSoftware.InnoSetup
powershell -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
```

`fetch-deps.ps1` is idempotent: it skips anything already unpacked. Downloads are cached in `third_party\dl\`.

## Build

```powershell
python build.py            # release build -> build\AirGlass.exe, build\airglass_test.exe, FFmpeg DLLs
python build.py --debug    # -O0 -g
python build.py --clean    # wipe build\ first
python build.py --pro      # personal build with AirGlass Pro always unlocked
```

The build is parallel and incremental. A source file recompiles when it changes, or when any project header changes
(C++ only). `--pro` uses its own object folder (`build\obj-pro`), so free and Pro objects never mix.

Output:

| File | What |
|---|---|
| `build\AirGlass.exe` | the app (static C++ runtime, GUI subsystem, DPI-aware manifest, icon, version info) |
| `build\airglass_test.exe` | test tool: loopback AirPlay sender and self-tests (see [Development](development.md)) |
| `build\avcodec-*.dll`, `avutil-*.dll`, `swresample-*.dll` | FFmpeg runtime (`avformat` is only needed by the test tool) |

## Install locally

```powershell
powershell -ExecutionPolicy Bypass -File deploy.ps1          # %LOCALAPPDATA%\Programs\AirGlass, Start menu, autostart, firewall rule
powershell -ExecutionPolicy Bypass -File uninstall.ps1       # removes it again (settings stay in %LOCALAPPDATA%\AirGlass)
```

`deploy.ps1` quits the running copy gracefully, so it sends its mDNS goodbye. Then it copies the files and restarts it
in the tray.

## Package a release

```powershell
powershell -ExecutionPolicy Bypass -File package.ps1 -Version 1.2.0
```

This builds `dist\AirGlass-Setup-1.2.0.exe` (Inno Setup, per-user, no admin) and `dist\AirGlass-1.2.0-source.zip`
(`git archive` of `HEAD`; the GPL requires the source to go with every copy). It refuses to run until the Lemon Squeezy
store settings in `src\license.h` are filled in. Normally you don't run this by hand: see [Releasing](releasing.md).

## Compiler flags worth knowing

* C++20, `-O2`, `-Wall -Wextra`. The build is expected to be warning-free.
* `UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, targeting Windows 10 (`_WIN32_WINNT=0x0A00`).
* Third-party C (`playfair`, `ed25519`) builds with `-w`.
* Linked statically (`-static`) apart from FFmpeg and system DLLs.
