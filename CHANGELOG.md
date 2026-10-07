# Changelog

All notable changes to AirGlass are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and AirGlass uses [Semantic Versioning](https://semver.org).

## [Unreleased]

### Added
- Steam-style refund counter: after unlocking Pro, the tray menu shows how many mirroring sessions have been used
  and whether a refund is still available (within 14 days and fewer than 10 sessions).

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
