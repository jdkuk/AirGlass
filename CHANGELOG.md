# Changelog

All notable changes to AirGlass are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and AirGlass uses [Semantic Versioning](https://semver.org).

## [Unreleased]

## [1.0.6] - 2026-11-15

### Added
- Rename this PC: right-click the tray icon → **Rename this PC…** to name the receiver after the room. Devices see
  the new name straight away, without restarting AirGlass.
- A machine-wide receiver name for IT departments: `name=` under `[receiver]` in
  `%ProgramData%\AirGlass\airglass.ini` wins over the user's setting ([Deployment](docs/deployment.md)).
- Pro: OBS mode. Press <kbd>O</kbd> (or use the tray or window menu) for a window OBS Window Capture handles well:
  opaque, chroma-key green (#00B140) round a plain rectangular picture, no glass, shadow or controls, and always
  titled "AirGlass (OBS)" so the capture source finds it again. AirGlass remembers the choice.
- Pro: region zoom. Press <kbd>Z</kbd> and drag a rectangle (or <kbd>Ctrl</kbd>+drag) to zoom smoothly into that
  part of the mirrored picture, up to 8x; drag again to zoom further. <kbd>Z</kbd>, <kbd>Esc</kbd> or a double-click
  zooms back out. Screenshots and OBS mode show the zoomed view; recordings keep the whole picture.
- Pro: draw on screen. Press <kbd>D</kbd> (or use the tray or window menu) and drag to draw on the mirrored picture
  with a pen, an arrow (<kbd>A</kbd>) or a highlighter (<kbd>H</kbd>; <kbd>P</kbd> is the pen) in red, yellow, green
  or blue (<kbd>1</kbd>–<kbd>4</kbd>). <kbd>Backspace</kbd> undoes the last stroke and <kbd>C</kbd> clears them all.
  Strokes stay on the picture when the window moves, resizes or zooms, and clear on a new session or a rotation.
  Screenshots and OBS mode show them; recordings copy the sender's stream, so they don't.
- Pro: an eraser (<kbd>E</kbd>) removes the strokes you drag it over, and a toolbar on the picture while drawing
  (pen, arrow, highlighter, eraser, the four colours, undo, clear all and ✓ to stop) makes drawing work with the
  mouse alone. It sits along the bottom of the picture, or down its side for a portrait phone; clicks on it never
  draw, and screenshots and OBS mode don't show it. <kbd>Backspace</kbd> now also takes back an erase or a clear.
- Pro: media controls for the mirroring device. <kbd>Space</kbd> plays or pauses, <kbd>Ctrl</kbd>+<kbd>→</kbd> /
  <kbd>Ctrl</kbd>+<kbd>←</kbd> skip to the next / previous track and <kbd>Ctrl</kbd>+<kbd>↑</kbd> /
  <kbd>Ctrl</kbd>+<kbd>↓</kbd> turn the device's volume up / down, through its AirPlay remote control (DACP, as with
  music). The tray has the same under **Media controls** while the device offers it; a device that doesn't gets a
  balloon saying so. TV Mode's `media` command also takes `volumeup` and `volumedown`.
- Pro: allow-list. Tray → **Ask before new devices connect**: when a device AirGlass doesn't know starts mirroring
  or audio, the PC asks "Allow … to use AirGlass?" with Allow once, Always allow or Block. No answer within 30 s
  turns it away; a turned-away device gets a clean refusal and shows an error. Answers are kept under `[devices]` in
  `config.ini`; tray → **Forget allowed and blocked devices** clears them. IT departments can turn it on for the
  whole machine with `askNewDevices=1` under `[security]` in `%ProgramData%\AirGlass\airglass.ini`.
- Pro: require a code to connect. Tray → **Require a code to connect**: an iPhone, iPad or Mac connecting for the
  first time shows Apple's "AirPlay Code" prompt, and the PC shows a random 4-digit code in a card on top of
  everything, on the screen it mirrors to. Each attempt gets a new code; three wrong codes in a row pause new codes
  (30 s, growing up to an hour). A device that typed the right code is remembered by its pairing key under
  `[devices]` in `config.ini` and isn't asked again; tray → **Forget paired devices** clears them. Devices that
  haven't paired are turned away with a clean refusal before any session starts. IT departments can turn it on for
  the whole machine with `requirePin=1` under `[security]` in `%ProgramData%\AirGlass\airglass.ini`. The pairing is
  AirPlay's legacy PIN exchange (SRP-6a, as UxPlay's `-pin`), with AirGlass's own SRP and AES-GCM code, tested
  against the RFC 5054 vectors and end to end in CI.
- A device that asks for a code by itself (some company-managed iPhones do) now gets one, in every edition, instead
  of failing to pair.
- Pro: two devices at once. An iPhone and an iPad (or two phones, or a Mac) can mirror at the same time, each in its
  own glass window, with its own decoder and recorder; the second window opens next to the first. A third device
  takes over the oldest of the two sessions. The free edition still mirrors one device at a time: a new device takes
  over, and the first time a different device does, AirGlass offers Pro. The tray's tools (screenshot, recording,
  OBS mode, zoom, drawing) and the keys act on the window used last; the tray says which, and
  **Switch to "…"** (<kbd>Tab</kbd> in a window) goes to the other one. Only one window's sound plays: the one used
  last, or else the newest session; each recording still gets its own session's sound. AirPlay music works as
  before. TV Mode drives the most recent session's window; under TV Mode the earlier window moves to the other
  corner. In OBS mode the second window is titled "AirGlass (OBS 2)".

- Optional read-only status over HTTP on the AirPlay port (`GET /status`, long-poll, cover art), for LAN apps such as a
  TV launcher. Off unless `[status] http=1` (see docs/status-http.md). With two devices mirroring, `mirror` is the
  most recent session (as TV Mode) and `sessions` lists both.
- `[window] full=1` opens every session full screen, in Pro too (for a PC whose screen is a TV); with two devices,
  both windows.

### Changed
- Pro: recordings now have sound. The mirrored session's audio is encoded to AAC (FFmpeg's built-in encoder) as a
  second track, kept in sync with the picture by the same clock; the picture is still copied without re-encoding.
  Sound that starts late or pauses becomes silence. Every recording gets the sound track, even one started before
  any sound plays: it is silent (44.1 kHz stereo, as iOS mirroring sends) until the sound starts, then the sound
  joins in sync. Sound at another rate starting later leaves the rest of the recording silent (logged).
- The AirGlass Pro dialog now lists what Pro includes: the floating glass window, screenshots, recording with sound,
  drawing, region zoom, OBS mode and the allow-list. The trial's welcome balloon names the new shortcuts.

### Fixed
- A mirroring window now gets the keyboard focus when it opens, so its keys (S, R, D, Z, O…) work without a click.
  Windows refuses the focus to a background program, so the keys used to go to the desktop. Not under TV Mode, not
  when a new device takes over a window already on screen, and not away from the other mirroring window.
- Pro: a recording started while the phone's screen was still is now as long as it ran. A device sends no frames
  while nothing moves, and the frames AirGlass keeps from before the click (to start on a keyframe) were timed by
  an MP4 edit list that some players ignore: a 7.5 s recording played as two minutes. Those frames are now packed
  into the file's first instant, each picture lasts until the next one, and the last one runs to the moment you
  stop, so a still screen gives a recording of the right length showing its picture. The sound needs no edit list
  either. The log's "finished" line and the tray's timer count the time since the click.
- A mirroring session now keeps the display awake, and wakes a display that had idled off when it starts
  (`[display] keepawake` / `wake` turn this off). With two devices, the display stays awake until both have stopped.

## [1.0.5] - 2026-11-15

### Added
- Free 7-day AirGlass Pro trial, starting with the first mirroring session. The tray menu shows the days left, and
  when the trial ends AirGlass offers Pro once and returns to full-screen mirroring.
- Pro: screenshots. Press <kbd>S</kbd> (or use the tray menu) to save a PNG of the glass window, with its real
  transparency, to Pictures\AirGlass and put the mirrored picture on the clipboard.
- Pro: recording. Press <kbd>R</kbd> to record the mirrored screen to an MP4 in Videos\AirGlass. The sender's H.264
  stream is copied without re-encoding, so it keeps full quality and uses almost no CPU. Picture only, no sound; a
  recording finishes by itself if the device rotates.
- Tray menu: Save screenshot, Start/Stop recording (with the running time) and Open screenshots folder.

### Changed
- The tray menu's "Unlock windowed mode" is now "Buy AirGlass Pro".

## [1.0.3] - 2026-10-20

### Changed
- AirGlass Pro is now $9.99 (about £7.99; buyers see their local currency), still a one-time purchase.

## [1.0.2] - 2026-10-09

### Fixed
- iPhone mirroring in portrait showed up as a wide landscape window with black bars: iOS sends portrait screens
  inside a 1920x1080 frame. AirGlass now detects the side bars and crops them, so the window is phone-shaped again,
  and it switches back to landscape when the phone rotates.

## [1.0.1] - 2026-10-09

### Added
- Welcome window on first launch after installing: confirms AirGlass is running, explains how to connect from an
  iPhone, iPad or Mac, points to the tray icon by the clock, and offers "Start AirGlass with Windows".
- The tray icon is shown next to the clock by default on Windows 11 instead of being hidden in the overflow menu.
- Steam-style refund counter: after unlocking Pro, the tray menu shows how many mirroring sessions have been used
  and whether a refund is still available (within 14 days and fewer than 10 sessions).

### Changed
- AirGlass Pro is now on sale through Lemon Squeezy; the app and website link straight to the checkout.

## [1.0.0] - 2026-10-07

First public release.

### Added
- AirPlay Screen Mirroring from iPhone, iPad and Mac (H.264, hardware decoded with D3D11VA, software fallback).
- Lossless AirPlay music: ALAC 16-bit/44.1 kHz decoded bit-exact, 128-tap windowed-sinc resampling, resend requests.
- Liquid Glass window: refractive bezel, spring animations at the display refresh rate (up to 120 Hz), rotation morph,
  smooth zoom, keep on top, full screen.
- Own mDNS/DNS-SD responder (no Bonjour install), with automatic renaming when the name is taken.
- DACP remote control of the sender (play/pause, next, previous, volume) and Now Playing metadata with artwork.
- Control pipe API (`\\.\pipe\AirGlass.Control`) for living-room shells.
- Free edition (full-screen mirroring) and **AirGlass Pro** (floating window) unlocked with a Lemon Squeezy license key.
- Per-user installer (Inno Setup), with a firewall rule and autostart.

### Changed
- Mirroring is requested at up to 1080p: iPhones mirror 4K landscape at only about 28 fps, and 1080p at about 60 fps.
- The mirroring audio buffer starts at 90 ms, which avoids early drop-outs on real devices.

[Unreleased]: https://github.com/jdkuk/AirGlass/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/jdkuk/AirGlass/releases/tag/v1.0.0
