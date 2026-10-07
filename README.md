# AirGlass

**The best-looking, best-sounding AirPlay receiver for Windows.**

AirGlass lets an iPhone, iPad or Mac mirror its screen, or play music in lossless quality, to a
Windows PC. It sits in the tray. When a device starts Screen Mirroring, a Liquid Glass window
springs in with the stream, and it melts away when mirroring stops.

* **Lossless music:** music sent over AirPlay arrives as Apple Lossless (ALAC, 16-bit/44.1 kHz)
  and plays bit-for-bit.
* **Smooth:** the window and every animation run at your display's refresh rate (up to 120 Hz), and
  video is hardware decoded.
* **Small and native:** a single C++ app with no Bonjour install, no .NET and no bundled extras.

## Use it

* **Screen Mirroring (iPhone / iPad / Mac):** Control Center → Screen Mirroring (two overlapping
  rectangles) → your PC's name.
* **Music in the best quality:** in the Music app (or Control Center's Now Playing) tap the AirPlay
  icon → your PC's name. Music sent this way arrives as Apple Lossless and is decoded bit-for-bit.
  Screen Mirroring carries sound too, but the device always compresses mirroring audio (AAC-ELD),
  so use the AirPlay audio picker for music.

The receiver uses this PC's name. If another AirPlay device already uses that name, AirGlass adds
" PC" to it. To choose a different name, set `name=` under `[receiver]` in
`%LOCALAPPDATA%\AirGlass\config.ini`, then restart AirGlass.

| In the window | |
|---|---|
| Drag anywhere | move |
| Drag an edge / corner | resize (keeps the device's shape) |
| Mouse wheel | smooth zoom around the cursor |
| Double-click, **F** or **F11** | full screen (Esc leaves) |
| Hover | glass controls: ✕ disconnect · 📌 keep on top · ⤢ full screen |
| **P** | keep on top |
| Right-click | menu |

Rotating the device morphs the window between portrait and landscape. Audio plays through the
default Windows output, and the device's volume buttons control it.

Tray icon (right-click): *Start with Windows*, *Open log folder*, *Quit*.

## Sound quality

* Music (AirPlay audio): Apple Lossless at about 0.9 Mbit/s for CD-quality 1.4 Mbit/s PCM, the
  most AirPlay sends to a receiver like this, decoded bit-exact.
* One conversion only: AirGlass plays in your output device's own format. Its 128-tap
  windowed-sinc converter (flat to 19.8 kHz, >105 dB clean) does the 44.1 → 48 kHz step and follows
  the device's clock with inaudible corrections. For a conversion-free path, set the output device
  to *44.1 kHz* in Windows Sound settings → device → Advanced.
* Music buffers about 0.25 s and re-requests lost Wi-Fi packets, so drop-outs are rare. Mirroring
  keeps a short buffer for lip sync.

## If your PC doesn't show up on the device

1. The device and the PC must be on the same network. Wi-Fi and Ethernet on the same router is fine.
2. Windows Firewall: the installer adds the rule **AirGlass (AirPlay receiver)**. If it's missing,
   allow AirGlass when Windows asks and tick **both Private and Public** networks.
3. VPNs: allow LAN access / local network discovery while the VPN is connected (in NordVPN, also
   turn *Invisibility on LAN* off).
4. Guest Wi-Fi and routers with "client isolation" block AirPlay entirely.
5. Logs: `%LOCALAPPDATA%\AirGlass\airglass.log` (set `debuglog=1` under `[app]` for detail).

## Notes

* Video: H.264, hardware decoded (D3D11VA) and presented the moment it's decoded. AirGlass
  advertises 120 fps to the sender. Apple devices choose their own rate (usually up to 60 fps),
  and the log reports it.
* No PIN or password: any device on your network can mirror to it, like an Apple TV set to
  "Allow access: Everyone on the same network".
* Not supported: AirPlay *video casting* from apps (YouTube's AirPlay button sends a link; use
  Screen Mirroring instead) and HEVC/4K mirroring.
* Control pipe: other apps on the same PC can place and drive the window over the named pipe
  `\\.\pipe\AirGlass.Control` (JSON lines; see `DESIGN.md`).

## Build from source

Needs Python 3, the Windows 10/11 SDK (for `fxc`), llvm-mingw unpacked to
`.toolchain\llvm-mingw-*` and the FFmpeg LGPL shared build unpacked to
`third_party\ffmpeg-*` ([BtbN builds](https://github.com/BtbN/FFmpeg-Builds/releases)).

```
python build.py                                           # output in build\
powershell -ExecutionPolicy Bypass -File deploy.ps1       # install for this user
powershell -ExecutionPolicy Bypass -File package.ps1      # installer + source zip in dist\
```

`build\airglass_test.exe` is a loopback AirPlay *sender* for testing: `stream` (mirroring),
`music` (lossless audio, metadata and remote control), `resampler`, `resolve`, `mdns`, `bplist`.

## License

AirGlass is free software under the **GNU General Public License v3.0** (see `LICENSE`), because it
includes GPL-3.0 code from UxPlay/RPiPlay. Paying for AirGlass gets you the signed installer,
updates and support; the source is always available to you under the GPL.
See `THIRD_PARTY_NOTICES.md` for the components it includes.

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. AirGlass is not affiliated with or
endorsed by Apple.
