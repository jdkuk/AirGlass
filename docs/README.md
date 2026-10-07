# AirGlass documentation

| Guide | For | What's inside |
|---|---|---|
| [Troubleshooting](troubleshooting.md) | Users | The PC doesn't appear, no sound, stutter, firewall, VPNs |
| [Licensing & editions](licensing.md) | Users, developers | Free vs Pro, license keys, offline use, the GPL |
| [Privacy](privacy.md) | Users | Exactly what leaves your PC |
| [Architecture](architecture.md) | Developers | Threads, D3D11 glass renderer, AirPlay protocol walkthrough |
| [Building](building.md) | Developers | Toolchain, dependencies, build switches |
| [Development & testing](development.md) | Developers | Loopback sender, headless snapshots, debug environment variables |
| [Control pipe API](control-pipe.md) | Integrators | JSON-over-named-pipe API to place and drive the window |
| [Releasing](releasing.md) | Maintainers | Versioning, tags, the release workflow, store settings |

## Source map

```
src/
  main.cpp              app shell: tray, config, edition, AirPlay events -> window, control pipe
  config.*              %LOCALAPPDATA%\AirGlass\config.ini
  license.*             AirGlass Pro: Lemon Squeezy activation/validation, upgrade dialog
  control_pipe.*        \\.\pipe\AirGlass.Control (JSON lines) + tiny JSON reader/writer
  crypto/               X25519/Ed25519 pairing, AES-CTR/CBC, SHA-512, FairPlay glue
  net/
    airplay_server.*    RTSP: /info, pair-setup/verify, fp-setup, SETUP/RECORD/TEARDOWN, SET_PARAMETER
    mdns.*              our own mDNS/DNS-SD responder and one-shot resolver
    mirror_stream.*     screen-mirroring TCP stream (AES-CTR -> H.264 Annex-B)
    audio_stream.*      RTP audio (AES-CBC), de-duplication, resend requests
    ntp_client.*        AirPlay timing
    dacp.*              remote control of the sender (play/pause/next...)
    bplist.*            binary property lists
  media/
    video_decoder.*     FFmpeg H.264 with D3D11VA (software fallback)
    audio_decoder.*     FFmpeg ALAC / AAC-LC / AAC-ELD
    audio_output.*      WASAPI, adaptive jitter buffer, drift tracking
    resampler.*         128-tap windowed-sinc resampler
  ui/
    glass_window.*      DirectComposition window, spring animations, input, TV placement
    compose.hlsl        the Liquid Glass compositor shader
    convert.hlsl        NV12 -> RGB
tools/                  airglass_test (loopback sender + self-tests), capture scripts, fetch-deps, brand renders
third_party/            playfair (GPL-3.0), ed25519 (zlib); FFmpeg is downloaded, not vendored
```
