# Troubleshooting

## Your PC doesn't appear in Screen Mirroring

1. **Same network.** The Apple device and the PC must be on the same network. Wi-Fi on the phone with Ethernet on the PC
   is fine as long as both connect to the same router.
2. **Firewall.** The installer adds the inbound rule **AirGlass (AirPlay receiver)** for all network profiles. If you
   declined it, either reinstall or allow AirGlass when Windows asks, and tick **both Private and Public**. Many home
   networks are classified as *Public*.
3. **VPN.** Allow LAN access / local network discovery while connected. In NordVPN, turn *Invisibility on LAN* off and
   enable *Allow LAN connections*.
4. **Guest Wi-Fi or "client isolation"** stops devices seeing each other, so AirPlay can't work there.
5. **Mesh and multicast.** Some routers drop multicast (mDNS). Look for "IGMP snooping" or "multicast enhancement" and try
   toggling it.
6. **Check the log.** Right-click the tray icon → *Open log folder* → `airglass.log`. A line like
   `mdns: first answer to 192.168.x.y (a device is looking for AirPlay receivers)` means your device *can* see AirGlass.

## Mirroring connects but is black

* Protected video (Netflix, Disney+, Apple TV+ and similar) is blanked by the sending app. This is by design.
* Otherwise, check the log for `video decoder` errors. AirGlass falls back to software decoding if the GPU decoder fails.

## Stutter or low frame rate

* The log reports the real rate every 10 s, e.g. `mirror: 58.7 fps, 7.4 Mbit/s, hardware decode`.
* Apple devices choose their own frame rate, up to 60 fps, and drop it on busy Wi-Fi. A 5 GHz network helps a lot.
* If you forced a 4K display in `config.ini`, remove it: iPhones mirror 4K landscape at about 28 fps.

## Sound problems

* **Crackles or drop-outs at the start of mirroring:** the jitter buffer grows by itself after a few underruns (the log
  says `audio: underrun #n, jitter target now … ms`). Persistent drop-outs usually mean congested Wi-Fi.
* **Music isn't lossless:** use the **AirPlay button** in the Music app, not Screen Mirroring. Mirroring audio is always
  AAC-ELD, because the device chooses it.
* **Wrong output:** AirGlass plays to the *default* Windows output device and follows it when it changes.

## Pro unlock problems

| Message | Fix |
|---|---|
| "Couldn't reach the license server" | Check the internet connection and retry. Only activation needs the internet. |
| "license_key not found" | Copy the key exactly from the receipt email (it looks like `XXXXXXXX-XXXX-…`). |
| "This license key has reached the activation limit" | A key works on 3 PCs. Deactivate an old PC from your order page, or contact support. |
| "This key is for a different product" | The key belongs to another product. |

## Reporting a bug

Open a [bug report](https://github.com/jdkuk/AirGlass/issues/new?template=bug_report.yml) and attach `airglass.log`. For
more detail, set `debuglog=1` under `[app]` in `config.ini`, restart AirGlass and reproduce the problem first.
