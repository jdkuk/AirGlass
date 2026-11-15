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

## Draw on screen (Pro, <kbd>D</kbd>)

| # | Steps | Expected |
|---|---|---|
| 1 | Mirror in the glass window, press <kbd>D</kbd>. | The cursor over the window becomes a red dot. A small dark pill at the bottom of the picture says "Pen · Drawing (D to exit)". Hovering shows no capsule. |
| 2 | Drag across the picture. | A smooth red line follows the mouse. The window does not move. |
| 3 | Press <kbd>2</kbd>, <kbd>3</kbd>, <kbd>4</kbd>, <kbd>1</kbd> and draw after each. | The cursor dot and the pill's dot change colour: yellow, green, blue, red; each new stroke uses that colour, old strokes keep theirs. |
| 4 | Press <kbd>A</kbd> and drag. Press <kbd>H</kbd> and drag over some text. Press <kbd>P</kbd> and drag. | An arrow with a filled head pointing where the drag ended; a wide see-through highlighter stroke (overlapping parts don't get darker); then the pen again. The pill names the tool. A plain click with the arrow leaves nothing; a click with the pen leaves a dot. |
| 5 | Quick double-click with the pen. | Two dots; the window doesn't go full screen. |
| 6 | Press <kbd>Backspace</kbd> twice, then <kbd>C</kbd>. | The last two strokes disappear one by one, then all of them. |
| 7 | Draw a few strokes, then move the window (press <kbd>D</kbd> to stop drawing, drag), resize it from a corner and use the mouse wheel. | The strokes stay on the same spots of the picture and scale with it. |
| 8 | Zoom into a region (<kbd>Z</kbd> then drag) over a stroke; draw while zoomed; zoom out. | The strokes zoom with the picture. A stroke drawn while zoomed in is thinner after zooming out. |
| 9 | Full screen (<kbd>F</kbd>) with strokes. | The strokes are on the same spots of the full-screen picture. The cursor stays visible as the dot while drawing. |
| 10 | Press <kbd>S</kbd> with strokes on and draw mode on. | The PNG and the clipboard picture contain the strokes but not the "Drawing (D to exit)" pill. |
| 11 | OBS mode (<kbd>O</kbd>) with strokes, draw mode on. | OBS captures the strokes; the pill is not shown in OBS mode. |
| 12 | Record (<kbd>R</kbd>) while drawing, stop, play the MP4. | The recording has no strokes (it is the sender's own stream). |
| 13 | Press <kbd>D</kbd> or <kbd>Esc</kbd>. | Drawing stops: arrow cursor, pill gone, the strokes stay; dragging moves the window again. <kbd>C</kbd> still clears. With a zoom, the first <kbd>Esc</kbd> zooms out, the next stops drawing. |
| 14 | With strokes on, rotate the phone; or end the session and mirror again. | The strokes are gone and draw mode is off. |
| 15 | Tray menu during mirroring; right-click in the window. | "Draw on screen  D" in both, ticked while drawing; the window menu also has "Clear drawing  C" when there are strokes. |
| 16 | Free edition: press <kbd>D</kbd>, or tray → "Draw on screen (Pro)…". | The AirGlass Pro dialog: "Drawing on screen is part of AirGlass Pro…". |

## Allow-list (Pro, tray → Ask before new devices connect)

| # | Steps | Expected |
|---|---|---|
| 1 | Tray menu. | "Ask before new devices connect", unticked. No "Forget…" item yet. |
| 2 | Choose it. | Ticked; balloon "AirGlass asks first". `askNewDevices=1` under `[security]` in `config.ini`. |
| 3 | Start mirroring from the iPhone. | Within a second the PC shows "Allow “<iPhone name>” to use AirGlass?" on top of everything, with a countdown from 30 s. The iPhone waits. |
| 4 | Click **Allow once**. | Mirroring starts. Stop it and mirror again: asked again. |
| 5 | Mirror again, click **Always allow**. | Mirroring starts. `config.ini` has `[devices]` with `<id>=allow <iPhone name>`. Stop and mirror again: no prompt. Play music to the PC (AirPlay audio): no prompt. |
| 6 | From a second device (iPad or Mac), mirror and click **Block**. | The device shows an error ("Unable to connect" or similar) instead of hanging. Try again: refused straight away, no prompt. `config.ini` has `<id>=block …`. |
| 7 | From the second device, after tray → **Forget allowed and blocked devices**, mirror and don't answer. | The countdown runs out; the prompt closes; the device shows an error within about 30 s. |
| 8 | Mirror from it again and close the prompt with ✕ or <kbd>Esc</kbd>. Then again and press <kbd>Enter</kbd>. | ✕/Esc turns it away this time (not remembered). Enter does nothing. |
| 9 | Mirror from it and cancel on the device while the prompt is open. | The prompt closes by itself within a second. |
| 10 | Mirror from the iPhone (always allowed) while a prompt for the iPad is open. | The iPhone connects; the iPad's prompt stays until answered. |
| 11 | Mirror from the iPhone full screen, then start mirroring from a new device. | The prompt appears over the full-screen picture; the iPhone keeps mirroring until you allow the new device. |
| 12 | Tray → untick "Ask before new devices connect". | New devices connect without a prompt; the list is kept. |
| 13 | As admin, create `%ProgramData%\AirGlass\airglass.ini` with `[security]` / `askNewDevices=1`, restart AirGlass. | The tray item is ticked and greyed, "(set by your organisation)". New devices are asked about, also in the free edition. Delete the file and restart: back to the user's choice. |
| 14 | Free edition (trial over) with it off: choose "Ask before new devices connect (Pro)…". | The AirGlass Pro dialog: "Asking before new devices connect is part of AirGlass Pro…". If it was turned on during the trial, it stays on and can be turned off. |
| 15 | Loopback test (`AirGlass.exe --loopback` + `airglass_test stream`) with it on. | No prompt; the test passes. |

## Recording with sound (Pro, <kbd>R</kbd>)

| # | Steps | Expected |
|---|---|---|
| 1 | Mirror from the iPhone, play a video with sound (or a YouTube clip with speech), press <kbd>R</kbd>. | Balloon "Recording the mirrored screen and its sound to Videos\AirGlass…". The log says `record: started …, sound 44100 Hz AAC` (or 48000). |
| 2 | Record about a minute, press <kbd>R</kbd>, play the MP4 (Films & TV, VLC, and drop it in a browser tab). | Picture and sound, lips in sync with speech from start to end. |
| 3 | Mirror with no sound playing, start recording, then after 10 s start a video with sound; stop after another 20 s. | The first 10 s are silent, then the sound comes in at the moment it started on the phone, in sync. |
| 4 | Pause the phone's video for 10 s mid-recording, then resume. | Silence for the pause, then sound in sync again (no drift after the gap). |
| 5 | Record for 15 minutes with speech. | Still in sync at the end (within about a frame or two). |
| 6 | Rotate the phone while recording. | The recording finishes by itself as before; its sound runs to the end of its picture. |
| 7 | If you have a sender that sets up no audio stream (its session has no `audio: … mirroring` line in the log), record from it. | Balloon "…(it sends no sound)…"; the MP4 has a picture track only, as in 1.0.5. |
| 8 | Check the file in File Explorer → Properties → Details, or `ffprobe`. | Video H.264 at the sender's size; audio AAC stereo 160 kb/s. |
| 9 | Free edition: press <kbd>R</kbd>. | The AirGlass Pro dialog, as before. |
