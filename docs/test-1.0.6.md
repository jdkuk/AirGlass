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

## OBS mode (Pro, <kbd>O</kbd>)

| # | Steps | Expected |
|---|---|---|
| 1 | Mirror in the glass window (Pro or trial), press <kbd>O</kbd>. | The glass, shadow and rounded corners go; the picture is a plain rectangle on solid green (#00B140) filling the whole window. Hovering shows no capsule. |
| 2 | Check the title (taskbar thumbnail, Alt+Tab). | "AirGlass (OBS)". |
| 3 | In OBS: Sources → + → Window Capture → window "[AirGlass.exe]: AirGlass (OBS)", Capture Method "Automatic" or "Windows 10 (1903 and up)". | The picture with a green border, no black or see-through areas. A Chroma Key filter (green) leaves just the picture. |
| 4 | Move, resize, mouse-wheel zoom, rotate the phone. | Still green and opaque throughout; OBS keeps capturing. |
| 5 | Press <kbd>F</kbd>. | Full screen, green letterbox bars. <kbd>Esc</kbd> returns. |
| 6 | End the session and mirror again. | The window opens in OBS mode again (remembered as `obs=1` under `[window]`), and OBS picks it up again by its title. |
| 7 | Tray menu during mirroring; right-click in the window. | Both show "OBS mode  O" with a tick; choosing it turns OBS mode off and the glass comes back, title "AirGlass". |
| 8 | Free edition (trial over, or loopback with `AIRGLASS_DEBUG_FREE=1`): press <kbd>O</kbd>, or tray → "OBS mode (Pro)…". | The AirGlass Pro dialog: "OBS mode is part of AirGlass Pro…". With `obs=1` left over from the trial, the free edition still shows the normal full-screen view. |

## Region zoom (Pro, <kbd>Z</kbd>)

| # | Steps | Expected |
|---|---|---|
| 1 | Mirror in the glass window, press <kbd>Z</kbd>. | The cursor over the window becomes a cross. |
| 2 | Drag a rectangle over part of the picture. | A white outline follows the drag and the rest of the picture dims. On release the view glides smoothly into that region; the window keeps its size and shape, and the picture isn't stretched. |
| 3 | Without pressing Z, hold <kbd>Ctrl</kbd> and drag a smaller rectangle. | Zooms further into the zoomed view (up to 8x). |
| 4 | Press <kbd>Z</kbd>. Zoom in again, press <kbd>Esc</kbd>. Zoom in again, double-click. | Each one glides back out to the whole picture. <kbd>Esc</kbd> while not zoomed still leaves full screen. |
| 5 | Press <kbd>Z</kbd> and just click (no drag). | Nothing zooms; the cursor goes back to the arrow. The window doesn't move. |
| 6 | Zoom in near a corner of the picture. | The view stays inside the picture (no black edge). |
| 7 | Zoom in, then press <kbd>S</kbd>. | The PNG and the clipboard picture show the zoomed view. |
| 8 | Zoom in with OBS mode on (<kbd>O</kbd>). | OBS captures the zoomed view on green. |
| 9 | Zoom in, then press <kbd>F</kbd> for full screen, and <kbd>Esc</kbd> twice. | The zoom carries over to full screen; the first <kbd>Esc</kbd> zooms out, the second leaves full screen. |
| 10 | Zoom in and rotate the phone; or end the session and mirror again. | The zoom resets to the whole picture. |
| 11 | Tray menu during mirroring; right-click in the window. | "Zoom into a region  Z" (or "Zoom out  Z" when zoomed) in both. |
| 12 | Free edition: press <kbd>Z</kbd>, or <kbd>Ctrl</kbd>+click the full-screen picture. | The AirGlass Pro dialog: "Region zoom is part of AirGlass Pro…". |
