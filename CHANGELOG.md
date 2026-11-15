# Changelog

All notable changes to AirGlass are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and AirGlass uses [Semantic Versioning](https://semver.org).

## [Unreleased]

## [1.0.6] - unreleased

### Added
- Rename this PC: right-click the tray icon → **Rename this PC…** to name the receiver after the room. Devices see
  the new name straight away, without restarting AirGlass.
- A machine-wide receiver name for IT departments: `name=` under `[receiver]` in
  `%ProgramData%\AirGlass\airglass.ini` wins over the user's setting ([Deployment](docs/deployment.md)).
- Pro: OBS mode. Press <kbd>O</kbd> (or use the tray or window menu) for a window OBS Window Capture handles well:
  opaque, chroma-key green (#00B140) round a plain rectangular picture, no glass, shadow or controls, and always
  titled "AirGlass (OBS)" so the capture source finds it again. AirGlass remembers the choice.

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
