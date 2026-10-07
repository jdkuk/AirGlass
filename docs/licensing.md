# Licensing & editions

## Free and Pro

| | Free | Pro ($2.99 one-time) |
|---|---|---|
| Screen Mirroring | full screen | full screen or floating glass window |
| Lossless AirPlay music | ✅ | ✅ |
| Move, resize, zoom, keep on top, corner float | | ✅ |
| PCs per key | | 3 |

In the free edition, every mirroring session opens full screen. Leaving full screen (<kbd>Esc</kbd>, <kbd>F</kbd>,
double-click, the capsule button, or a control-pipe `float` layout) opens the **AirGlass Pro** dialog instead.

## How the unlock works

```mermaid
sequenceDiagram
    participant U as User
    participant A as AirGlass
    participant L as Lemon Squeezy
    U->>L: Buy AirGlass Pro (checkout)
    L-->>U: Receipt email with license key
    U->>A: Tray → Unlock windowed mode → paste key
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
