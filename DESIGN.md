# AirGlass — design

A native Windows AirPlay screen-mirroring receiver. It lives in the tray, advertises
itself on the LAN, and when an iPhone / iPad / Mac starts Screen Mirroring to it, a
Liquid-Glass window pops up showing the stream. Stop mirroring and the window melts away.

Everything is one self-contained `AirGlass.exe` (C++20, Win32, D3D11, DirectComposition,
WASAPI) plus FFmpeg's `avcodec`/`avutil` DLLs for decoding. No Bonjour, no GStreamer, no
.NET, no admin rights needed to run.

---

## 1. User experience

| Moment | What the user sees |
|---|---|
| Idle | Nothing but a tray icon ("AirGlass — ready as *Living Room*"). |
| Phone picks "Living Room" in Screen Mirroring | Within ~100 ms a glass slab springs in (scale .92→1, fade, lift) in the phone's shape, showing a slow aurora gradient, a pulsing AirPlay glyph and "*Taylor's iPhone* · Connecting…". |
| First frame decodes | The video resolves out of a blur (≈350 ms), aspect-locked inside a thin refractive glass bezel. |
| Phone rotates | The window morphs portrait⇄landscape with a spring (centre fixed, long side preserved), video re-fits live. |
| Hover | A floating glass capsule drops in at the top: **✕ disconnect · 📌 keep on top · ⤢ full screen**. A liquid "droplet" lens follows the hovered button and merges into the capsule (smooth-min). Hides after 2.2 s of stillness. |
| Drag anywhere | Moves the window (native move loop, no snap resizing). |
| Drag edges/corners | Resizes, aspect-locked, content re-rendered synchronously every step. |
| Scroll wheel | Smoothly zooms the window around the cursor (spring animated). |
| Double-click / F / F11 | Animated morph to full screen (radius, bezel, shadow → 0, black letterbox). Esc or double-click returns. |
| Phone stops mirroring | Slab scales down, fades and blurs out; window hides. |
| ✕ / Alt+F4 | Disconnects the sender and dismisses. App keeps running in tray. |

The window is pure content: no title bar, no Windows chrome. Rounded "continuous" corners,
soft custom shadow, 1 px specular rim lit from the top-left, bezel refraction that picks up
the colours of the video edge (so the glass tints itself to whatever is on screen).

### 120 fps
* The compositor path is a flip-model DirectComposition swap chain with a frame-latency
  waitable object, presenting at the display's refresh (e.g. 3840×2160 @ 120 Hz).
* All animation is spring physics integrated on the render thread with real frame delta,
  so every UI motion (pop-in, morphs, hover lens, zoom, full-screen) runs at 120 Hz.
* Resizing never reallocates: buffers are allocated at monitor size and the live region is
  set with `IDXGISwapChain2::SetSourceSize`, and each resize step is rendered synchronously
  in `WM_SIZE` so the glass edge tracks the cursor with no black/stretched frames.
* Video frames are presented the instant they are decoded (hardware D3D11VA, zero-copy to
  a GPU texture). We advertise `maxFPS=120` and the display's refresh to the sender; the
  sender chooses its actual rate (Apple devices typically send ≤60 fps; the log reports
  the measured rate).
* When nothing moves and no new frame arrives the render thread sleeps (0% GPU idle).

## 2. Architecture

```
                ┌────────────── AirGlass.exe ───────────────────────────────┐
 iPhone/Mac     │  mDNS responder (UDP 5353, own implementation)            │
   ─ Bonjour ─▶ │    _airplay._tcp + _raop._tcp, TXT records, A per iface   │
                │                                                          │
   ─ RTSP ────▶ │  AirPlayServer (TCP 7000, dual-stack)                     │
                │   per-connection: /info, pair-setup, pair-verify (X25519  │
                │   + Ed25519), fp-setup (FairPlay SAP), SETUP/RECORD/...   │
                │     ├─ NtpClient      (UDP, timing requests every 3 s)    │
                │     ├─ MirrorStream   (TCP, AES-CTR → AVCC→AnnexB) ─┐     │
                │     └─ AudioStream    (UDP RTP, AES-CBC, dedupe)─┐  │     │
                │                                                  │  │     │
                │  AudioDecoder (FFmpeg AAC-ELD/AAC-LC/ALAC) ◀─────┘  │     │
                │     └─▶ AudioOutput (WASAPI, adaptive jitter buffer) │     │
                │  VideoDecoder (FFmpeg H.264, D3D11VA) ◀─────────────┘     │
                │     └─▶ GPU copy into NV12 texture (shared device lock)   │
                │                                                          │
                │  GlassWindow (UI thread + render thread)                  │
                │     D3D11 → NV12→RGB+mips → glass compose shader →        │
                │     DirectComposition swap chain → DWM @ display Hz       │
                │  Tray icon, config, log                                   │
                └──────────────────────────────────────────────────────────┘
```

### Threads
* **UI thread** — window procedure, tray, geometry decisions, synchronous renders on
  geometry change.
* **Render thread** — animation integration + compose + present, sleeps when idle.
* **Network** — accept thread, one thread per RTSP connection, mirror TCP thread (also
  decrypts + decodes), audio UDP thread (decrypt + decode), NTP thread, mDNS thread.
* **Audio** — WASAPI event thread (MMCSS "Pro Audio").

One D3D11 device (BGRA + video support, multithread-protected) is shared by the decoder,
D2D (text) and the renderer; a single recursive GPU lock serialises immediate-context use
(also handed to FFmpeg's D3D11VA lock callbacks).

## 3. Protocol (AirPlay 2 "legacy pairing" mirroring, as spoken by iOS/macOS)

1. **Discovery** — our own mDNS responder answers PTR/SRV/TXT/A for
   `<Name>._airplay._tcp.local` and `AABBCCDDEEFF@<Name>._raop._tcp.local`, host
   `<Name>-AirGlass.local`, announces 3× at start, goodbye on exit. Features
   `0x5A7FFEE6,0x0` (screen mirroring + audio + legacy pairing, no URL/HLS video).
2. **GET /info** — binary plist: deviceID, features, pk, name, model `AppleTV3,2`,
   displays[{width,height,refreshRate,maxFPS…}] (display = the monitor's shape capped at
   1920×1080, because iPhones mirror 4K landscape at only ~28 fps; refresh capped 120), audio formats/latencies.
3. **pair-setup** — exchange Ed25519 public keys (persistent receiver identity).
4. **pair-verify** — X25519 ECDH; AES-128-CTR keyed by SHA-512("Pair-Verify-AES-Key"‖S),
   IV by SHA-512("Pair-Verify-AES-IV"‖S); Ed25519 signatures both ways.
5. **fp-setup** — FairPlay SAP v2.5 two-step handshake (canned replies) and later
   `playfair_decrypt` of the 72-byte `ekey` → 16-byte AES key.
6. **SETUP #1** — ekey/eiv/timingPort → AES key := SHA-512(fairplayKey ‖ ecdhSecret)[0:16];
   start NTP client; reply timingPort/eventPort.
7. **SETUP streams** — type 110 (mirror): TCP dataPort; stream key/IV =
   SHA-512("AirPlayStreamKey|IV"+streamConnectionID ‖ aesKey)[0:16], one continuous
   AES-CTR keystream over all video payload bytes. Type 96 (audio): UDP data/control
   ports, ct=8 AAC-ELD (mirroring), ct=2 ALAC, ct=4 AAC-LC; AES-CBC per packet with eiv.
8. **Mirror packets** — 128-byte header (LE size, type 0 video / 1 SPS+PPS avcC /
   5 stats), video payload AVCC → Annex-B, SPS/PPS prepended after each codec packet.
9. **Teardown** — TEARDOWN 110 / TEARDOWN(all) / RTSP close / NTP silence (15 s) all end
   the session and dismiss the window.

## 4. Rendering (Liquid Glass)

Two passes per new video frame, one per UI frame:
1. **Convert** NV12 → RGBA8 (BT.601/709, limited/full from the stream), then
   `GenerateMips` (the mip chain doubles as a free blur pyramid for the glass).
2. **Compose** (single full-window pixel shader, premultiplied alpha):
   * soft analytic shadow of the slab,
   * slab = squircle rounded rect (superellipse corners),
   * video: 2×2 supersampled trilinear when shrinking, Catmull-Rom bicubic when enlarging,
     reveal = LOD bias animated from blurry to sharp,
   * **bezel glass**: for each rim pixel, find the video edge along the inward normal and
     sample deeper inside with a quadratic "wrap" (content bends round the rim), blurred
     from the mip pyramid, R/G/B offset for chromatic dispersion, saturation lift + milk,
     Blinn-Phong highlights from a key light (top-left) and fill light (bottom-right),
     Fresnel edge brightening, crisp 1 px rim line,
   * **controls capsule**: SDF capsule smooth-unioned with a hover droplet; content under
     it magnified ×1.1 and refracted near the edge, frosted (mip blur), adaptive tint and
     icon colour from the average luminance underneath (dark icons on bright video),
     specular rim, SDF-drawn icons (✕, pin, expand/collapse),
   * **connecting placeholder**: animated aurora blobs, SDF AirPlay glyph with pulse,
     device-name label (DirectWrite → texture).

## 5. Robustness
* Session take-over: a new sender replaces the current one.
* Dismiss on TEARDOWN, iOS 27 "full TEARDOWN without 110", mirror socket close or NTP
  timeout.
* Audio: 3× redundant ELD packets de-duplicated by sequence number, gaps concealed,
  resend requests for ALAC, adaptive-rate jitter buffer (±0.3 %), device-change recovery.
* Decoder: hardware D3D11VA with automatic software fallback.
* Log: `%LOCALAPPDATA%\AirGlass\airglass.log`. Config: `%LOCALAPPDATA%\AirGlass\config.ini`
  (`name=`, display override, persistent device id + Ed25519 key).

## 6. Deployment
* Installed to `%LOCALAPPDATA%\Programs\AirGlass`, Start-menu shortcut, starts with
  Windows (toggle in tray menu), single instance.
* Windows Firewall: one inbound allow rule for the exe (all profiles, because many home
  networks are classified as *Public*).

## 7. Verification
* Crypto self-tests (RFC 7748 X25519, RFC 8032 Ed25519, NIST AES-CTR/CBC, SHA-512).
* bplist reader/writer cross-checked against Python `plistlib`.
* `airglass_test.exe` — a loopback AirPlay *sender* that performs the full handshake
  (pair-verify, FairPlay, SETUP), then streams encrypted H.264 (portrait → landscape to
  exercise the rotation morph) and encrypted AAC, then tears down; plus an mDNS browse.
* `--loopback` mode (127.0.0.1 only, no mDNS) and a headless snapshot mode
  (`AIRGLASS_DEBUG_SNAPSHOT`, window stays hidden, frames written as images) for visual checks
  without popping windows: `tools\snapshots.ps1`, `tools\e2e.ps1`.

## 8. As built — decisions made during implementation
* **Swap chain sizing:** composition swap chains *stretch* their `SetSourceSize` region to the
  buffer size, so buffers are resized to the window (`ResizeBuffers`) instead.
* **FairPlay mode byte:** `playfair` reads the mode from byte 12 of the 164-byte key message;
  out-of-range values are rejected (they index past its tables).
* **Unique name:** at start-up the network is browsed and the receiver renames itself if the
  name is taken (e.g. a speaker already called "Office" makes the PC "Office PC").
* **Glass sizing:** bezel ≈ 0.85 % of the long side, corner radius ≈ 6 % of the short side,
  controls scale 0.72–1.3× with the picture.
* **Audio device** is opened only while an audio stream exists (an idle open stream keeps
  Windows awake).

## 9. Control pipe, music and audio quality (added later)

**Control pipe** `\\.\pipe\AirGlass.Control` (`src/control_pipe.*`): JSON lines both ways,
current-user-only DACL, remote clients rejected, multiple clients, a client that stops reading is
dropped after 500 ms. It lets a living-room shell ("TV Mode") place and drive the window. While a
client is connected the window is *TV-controlled*: topmost, `WS_EX_NOACTIVATE` + `MA_NOACTIVATE` (the remote
keeps driving TV Mode), placed by `layout {mode float|full|hidden, corner, size, margin}`, with a
4-button capsule (full · corner · size · stop) whose droplet follows `highlight`. Float size is
equal-area: a 16:9 picture gets 28/40/55 % of the screen width and other shapes the same area
(portrait capped at 62/80/100 % of the height), anchored to the corner with the margin; rotation
keeps the corner. Dragging/zooming by hand reports `free`. The capsule scales to a 1080p-equivalent
size on big screens. If TV Mode disconnects, the window keeps its place; after 5 s it reverts to
standalone behaviour (a TV-server restart doesn't flicker it).

**Music (audio-only AirPlay)**: an ALAC/AAC stream on a connection without a mirror stream is a
*music session* (no window). `SET_PARAMETER` carries DMAP track info (`minm/asar/asal/astm`),
`progress: start/current/end` (RTP time) and artwork (`image/jpeg|png`, written atomically to
`%LOCALAPPDATA%\AirGlass\nowplaying.*`). Play state comes from FLUSH (`RTP-Info` seq drops stale
packets) and packet flow. Position = progress anchor + elapsed while playing. The sender's
`DACP-ID`/`Active-Remote` headers are resolved with a one-shot mDNS SRV query for
`iTunes_Ctrl_<id>._dacp._tcp`, then `GET /ctrl-int/1/{playpause,nextitem,previtem,...}`.
Events are debounced (40 ms) plus a 5 s position heartbeat.

**Audio quality**
* iOS sends audio-only AirPlay to this receiver as ALAC 16/44.1 (realtime, lossless) because we
  advertise legacy pairing without buffered-audio support; AirPlay 2 *buffered* audio would be
  AAC 256k. The ALAC config (frame length, bit depth, rate) is built from the SETUP `audioFormat`
  bit and `spf`/`sr`, and packets are checked to be ALAC frames before decoding.
* Old path: Windows' own SRC (44.1→48 k) plus a linear-interpolation drift corrector, which acted
  as a moving 2-tap average (up to −6 dB at 19.5 kHz, 15 dB SNR at 10 kHz). New path: the WASAPI
  stream opens in the engine's mix format and `SincResampler` (128-tap Kaiser β=10 windowed sinc,
  1024 phases) does the single conversion plus drift tracking: 105–120 dB SNR, 0.000 dB gain from
  1 to 19.5 kHz (`airglass_test resampler`). Drift steering is slew-limited (music ≤ 0.05 %/s).
* Music buffers 230 ms (≈ the 11025-frame latency RECORD reports), waits up to 200 ms for
  retransmissions with up to three resend requests, and keeps timing with silence for a truly lost
  packet; the jitter target grows only when a stall recovers quickly (not on pause/end).

**Verification**: `airglass_test music` (iOS-like 352-sample ALAC frames, DMAP, artwork, progress,
FLUSH pause, retransmission service, fake DACP server) against `--loopback` with
`AIRGLASS_DEBUG_AUDIODUMP` (decoded PCM was bit-identical to the sent PCM) and
`AIRGLASS_DEBUG_DACP_PORT`; a Node pipe client checked the control-pipe events and commands end
to end.
