# Licensing & editions

## Free and Pro

| | Free | Pro ($9.99 one-time) |
|---|---|---|
| Screen Mirroring | full screen | full screen or floating glass window |
| Lossless AirPlay music | ✅ | ✅ |
| Rename this PC (tray menu), machine-wide name ([Deployment](deployment.md)) | ✅ | ✅ |
| Move, resize, zoom, keep on top, corner float | | ✅ |
| Screenshots (<kbd>S</kbd>): PNG to Pictures\AirGlass + the picture on the clipboard | | ✅ |
| Recording (<kbd>R</kbd>): MP4 to Videos\AirGlass, the sender's H.264 copied as-is, with the session's sound (AAC) | | ✅ |
| OBS mode (<kbd>O</kbd>): opaque window, chroma-key green (#00B140) round the picture, title "AirGlass (OBS)" | | ✅ |
| Region zoom (<kbd>Z</kbd> then drag, or <kbd>Ctrl</kbd>+drag): zoom into part of the mirrored picture | | ✅ |
| Draw on screen (<kbd>D</kbd>): pen, arrow, highlighter and eraser, with an on-screen toolbar; in screenshots and OBS mode, not in recordings | | ✅ |
| Allow-list (tray → *Ask before new devices connect*): Allow once / Always allow / Block for each new device | machine-wide setting only ([Deployment](deployment.md)) | ✅ |
| Require a code to connect (tray): a new device types the 4-digit *AirPlay Code* shown on the PC; paired devices are remembered | machine-wide setting only ([Deployment](deployment.md)) | ✅ |
| Devices mirroring at once, each in its own window | 1: a new device takes over | 2: a third device takes over the oldest session |
| PCs per key | | 3 |

**Free trial:** the first mirroring session starts a 7-day Pro trial (`license::kTrialDays`; `trial` in
`config.ini`). The tray menu shows the days left. When it ends, AirGlass shows the AirGlass Pro dialog once and then
behaves as the free edition. Loopback tests and `--pro` builds skip the trial.

In the free edition, every mirroring session opens full screen. Leaving full screen (<kbd>Esc</kbd>, <kbd>F</kbd>,
double-click, the capsule button, or a control-pipe `float` layout) opens the **AirGlass Pro** dialog instead.

**Two devices at once:** Pro has two mirroring windows, so two senders can mirror at the same time; a third takes
over the oldest session (its window and all). The free edition has one, and a new sender takes it over, as in 1.0.5;
the first time a *different* device takes over in a run, AirGlass offers Pro ("Mirroring two devices at once is part
of AirGlass Pro."). If Pro ends while two are mirroring (trial over, key refunded), both keep going until the next
device connects, which then takes over both. Allow-list, codes, trial and refund counting stay per connection.

## How the unlock works

```mermaid
sequenceDiagram
    participant U as User
    participant A as AirGlass
    participant L as Lemon Squeezy
    U->>L: Buy AirGlass Pro (checkout)
    L-->>U: Receipt email with license key
    U->>A: Tray → Buy AirGlass Pro → paste key
    A->>L: POST /v1/licenses/activate (key, "AirGlass on <PC name>")
    L-->>A: activated, instance id, store/product
    A->>A: check store + product, save key + instance in config.ini
    Note over A: Pro unlocked, works offline from now on
    A->>L: each start (after 15 s): POST /v1/licenses/validate
    L-->>A: valid / invalid
    Note over A: only a definite "invalid" (refund, disabled key) relocks;<br/>being offline keeps Pro
```

* Code: [`src/license.h`](../src/license.h) and [`src/license.cpp`](../src/license.cpp) (WinHTTP, no extra libraries).
  The upgrade dialog is an in-memory Win32 dialog template.
* Only the license key and a label (`AirGlass on <PC name>`) are sent, and only to `api.lemonsqueezy.com`. See
  [Privacy](privacy.md).
* `store_id` and `product_id` in the response are checked against `kStoreId` and `kProductId`, so a key sold for another
  Lemon Squeezy product can't unlock AirGlass.
* Test it from the command line: `airglass_test license activate <key> <name>` and
  `airglass_test license validate <key> <instance-id>`.

## Refunds

The policy works like Steam's: a full refund **within 14 days of purchase, if Pro was used for fewer than 10
mirroring sessions**. Statutory consumer rights apply regardless.

* After a key is activated, AirGlass counts mirroring sessions (`[license] unlocked` and `sessions` in `config.ini`).
  The tray menu shows `Refundable: 3 of 10 sessions used, 9 days left`, or `refund period over`.
* The count lives only on the buyer's PC, so it's informational: refunds are granted by hand in the Lemon Squeezy
  dashboard. Disabling the refunded key relocks AirGlass on its next start.
* The limits are `license::kRefundDays` and `license::kRefundSessions` in [`src/license.h`](../src/license.h).
  Loopback test sessions and personal `--pro` builds aren't counted.

## Open source and the GPL

AirGlass is licensed under the **GNU GPL-3.0**, because it includes GPL-3.0 code from UxPlay/RPiPlay (the FairPlay
handshake). In practice:

* The source is public, and every release ships with its source zip.
* Anyone may build AirGlass themselves, including with `python build.py --pro`, which skips the license check. That's
  allowed by the GPL, and it's fine with us.
* Pro buyers pay for the signed, ready-made installer, updates and support, and help keep the project alive.

## Personal builds

`python build.py --pro` defines `AIRGLASS_ALWAYS_PRO`, so Pro is always on and no key is needed. Use it for your own
machines and development. Release builds never set it.
