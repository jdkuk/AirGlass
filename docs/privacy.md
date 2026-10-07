# Privacy

AirGlass is designed to keep everything on your PC and your local network.

| Data | Where it goes |
|---|---|
| Screen Mirroring video and audio | Your local network only (device → PC). Never stored, never uploaded. |
| AirPlay music, track info, artwork | Local network only. The current artwork is cached in `%LOCALAPPDATA%\AirGlass` and replaced by the next track. |
| Your device's name and model | Shown in the window and written to the local log file. |
| Receiver identity (device ID, Ed25519 key) | Generated on first run and stored in `config.ini`. Never leaves the PC except in the AirPlay handshake on your network. |
| **Pro license key** | Sent to **Lemon Squeezy** (`api.lemonsqueezy.com`) when you activate it, and checked once per start, together with the label `AirGlass on <PC name>`. |

There's **no** telemetry, analytics, crash reporting, update ping or account.

Purchases are handled by Lemon Squeezy as merchant of record, under
[their privacy policy](https://www.lemonsqueezy.com/privacy). AirGlass never sees your payment details.

Logs (`%LOCALAPPDATA%\AirGlass\airglass.log`) stay on your PC. They're only shared if you attach them to a bug report
yourself. Read them first: they contain your device names and local IP addresses.
