# AirGlass 1.0.6 manual test checklist

Run on a real PC with an iPhone (and an iPad if you have one). Each step lists what should happen.

## Rename this PC (free and Pro)

| # | Steps | Expected |
|---|---|---|
| 1 | Right-click the tray icon. | The menu has **Rename this PC…** under "Receiving as “…”". |
| 2 | Choose it, type `Living Room`, press Enter. | Balloon "Renamed". The tray tooltip and the menu header say “Living Room”. |
| 3 | On the iPhone, open Control Centre → Screen Mirroring (close and reopen it if it was open). | The PC is listed as **Living Room**, once, without restarting AirGlass. |
| 4 | Mirror to it. | Mirroring works as before. |
| 5 | Rename to the name of another AirPlay device on the network (e.g. an Apple TV). | AirGlass uses that name + " PC" and the balloon says so. |
| 6 | Rename with an empty name, or only dots. | The dialog beeps and stays open. Cancel leaves the name unchanged. |
| 7 | Quit and restart AirGlass. | The new name is kept (`name=` under `[receiver]` in `%LOCALAPPDATA%\AirGlass\config.ini`). |
| 8 | As admin, create `%ProgramData%\AirGlass\airglass.ini` with `[receiver]` / `name=Meeting Room 2`, restart AirGlass. | The PC appears as **Meeting Room 2**. The tray menu shows a greyed "Name set by your organisation" and no Rename item. The user's `config.ini` name is untouched. |
| 9 | Delete that file and restart. | The user's own name is back. |
