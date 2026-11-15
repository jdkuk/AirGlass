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
| 1 | Mirror in the glass window, press <kbd>D</kbd>. | The cursor over the window becomes a red dot. A dark see-through toolbar with a light rim appears on the picture (along the bottom for a landscape picture, down the left side for a portrait phone): pen (marked), arrow, highlighter, eraser · red (ringed), yellow, green, blue · undo, bin (both dimmed with no strokes) · ✓. Hovering shows no capsule. |
| 2 | Drag across the picture. | A smooth red line follows the mouse. The window does not move. |
| 3 | Press <kbd>2</kbd>, <kbd>3</kbd>, <kbd>4</kbd>, <kbd>1</kbd> and draw after each. | The cursor dot and the toolbar's ringed colour change: yellow, green, blue, red; each new stroke uses that colour, old strokes keep theirs. |
| 4 | Press <kbd>A</kbd> and drag. Press <kbd>H</kbd> and drag over some text. Press <kbd>P</kbd> and drag. | An arrow with a filled head pointing where the drag ended; a wide see-through highlighter stroke (overlapping parts don't get darker); then the pen again. The toolbar marks the tool. A plain click with the arrow leaves nothing; a click with the pen leaves a dot. |
| 5 | Quick double-click with the pen. | Two dots; the window doesn't go full screen. |
| 6 | Press <kbd>Backspace</kbd> twice, then <kbd>C</kbd>, then <kbd>Backspace</kbd>. | The last two strokes disappear one by one, then all of them, then they all come back. |
| 7 | Draw a few strokes, then move the window (press <kbd>D</kbd> to stop drawing, drag), resize it from a corner and use the mouse wheel. | The strokes stay on the same spots of the picture and scale with it. |
| 8 | Zoom into a region (<kbd>Z</kbd> then drag) over a stroke; draw while zoomed; zoom out. | The strokes zoom with the picture. A stroke drawn while zoomed in is thinner after zooming out. |
| 9 | Full screen (<kbd>F</kbd>) with strokes. | The strokes are on the same spots of the full-screen picture. The cursor stays visible as the dot while drawing. |
| 10 | Press <kbd>S</kbd> with strokes on and draw mode on. | The PNG and the clipboard picture contain the strokes but not the toolbar. |
| 11 | OBS mode (<kbd>O</kbd>) with strokes, draw mode on. | OBS captures the strokes; the toolbar is not shown in OBS mode (and clicking where it was draws). |
| 12 | Record (<kbd>R</kbd>) while drawing, stop, play the MP4. | The recording has no strokes (it is the sender's own stream). |
| 13 | Press <kbd>D</kbd> or <kbd>Esc</kbd>. | Drawing stops: arrow cursor, toolbar gone, the strokes stay; dragging moves the window again. <kbd>C</kbd> still clears. With a zoom, the first <kbd>Esc</kbd> zooms out, the next stops drawing. |
| 14 | With strokes on, rotate the phone; or end the session and mirror again. | The strokes are gone and draw mode is off. |
| 15 | Tray menu during mirroring; right-click in the window. | "Draw on screen  D" in both, ticked while drawing; the window menu also has "Clear drawing  C" when there are strokes. |
| 16 | Free edition: press <kbd>D</kbd>, or tray → "Draw on screen (Pro)…". | The AirGlass Pro dialog: "Drawing on screen is part of AirGlass Pro…". |
| 17 | Draw a few strokes, press <kbd>E</kbd> and drag across two of them. | The cursor is an empty white ring; the two strokes vanish as the ring touches them (whole strokes), the others stay. The toolbar marks the eraser and no colour is ringed. <kbd>Backspace</kbd> brings both back in place. |
| 18 | With the eraser, press <kbd>3</kbd> (or click green on the toolbar). | Back to the pen, in green. |
| 19 | Mouse only: click each toolbar button in turn (pen, arrow, highlighter, eraser, the colours), drawing after each; click undo; click the bin; click undo again; click ✓. | Each click changes the tool or colour (the cursor changes over the picture; over the toolbar it is a hand) and never leaves a dot or a stroke; undo takes back the last stroke; the bin clears all; undo brings them back; ✓ stops drawing. Quick double-clicks on undo take back two strokes. Starting a drag on the toolbar draws nothing. |
| 20 | Small window (mouse wheel down to the smallest), portrait and landscape. | The toolbar shrinks to fit inside the picture; on a very small picture it is left out (keys still work). |
| 21 | Full screen (<kbd>F</kbd>) in draw mode. | The toolbar sits on the full-screen picture, at its normal size. |

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

## Require a code to connect (Pro, tray → Require a code to connect)

Use an iPhone that has never connected to this PC (or tray → **Forget paired devices** first). Keep the log open
(tray → Open log folder → `airglass.log`).

| # | Steps | Expected |
|---|---|---|
| 1 | Tray menu. | "Require a code to connect", unticked, under the allow-list items. No "Forget paired devices" yet. |
| 2 | Choose it. | Ticked; balloon "AirGlass asks for a code". `requirePin=1` under `[security]` in `config.ini`. Log: `pin: require a code to connect: on`. |
| 3 | On the iPhone, close and reopen Screen Mirroring and pick the PC. | Within a second or two the iPhone shows **AirPlay Code** with four boxes, and the PC shows a dark card "AirPlay code" with four large digits, on top of everything, centred on the screen the mouse is on (or the mirroring window's screen). The card doesn't take the keyboard focus. |
| 4 | Type a wrong code on the iPhone. | The iPhone says the code is wrong (or "Unable to connect"); the card goes. Log: `pair-setup-pin: wrong code`. Try again: a **new** code on the PC. |
| 5 | Type the right code. | The card goes and mirroring starts. Log: `paired with the code (…)` and `pin: "…" paired with the code; remembered`. `config.ini` `[devices]` has `PIN:<64 hex digits>=paired <iPhone name>`. |
| 6 | Stop mirroring, mirror again; restart AirGlass and mirror again; play music to the PC. | No code asked any of these times. |
| 7 | From a second device (iPad or Mac) that never paired, pick the PC, then cancel the code prompt on the device. | A code appears, and the card goes within a few seconds of cancelling (or after 2 minutes at the latest). |
| 8 | From the second device, type a wrong code three times in a row, then try once more. | The fourth attempt is refused straight away with no code shown (the device shows an error). Log: `3 wrong codes in a row; no new codes for 30 s`. After 30 s a code is shown again. |
| 9 | Wait more than 2 minutes after a code appears before typing it. | The card has gone by itself; the code is refused; the next attempt shows a new code. |
| 10 | Tray → **Forget paired devices**. Mirror from the iPhone from step 5. | Balloon "Paired devices forgotten". The iPhone is asked for a code again (**device test:** if the iPhone shows only an error instead of the code prompt, note exactly what it shows and whether a second try brings the prompt). Log: `pair-verify from a sender that hasn't paired with the code`. |
| 11 | Turn on *Ask before new devices connect* too, and mirror from a device that hasn't paired. | First the code; after the right code the PC asks "Allow … to use AirGlass?". |
| 12 | Untick "Require a code to connect". Mirror from a new device. | No code; it connects (subject to the allow-list). Paired devices stay listed in `config.ini`. |
| 13 | As admin, create `%ProgramData%\AirGlass\airglass.ini` with `[security]` / `requirePin=1`, restart AirGlass. | The tray item is ticked and greyed, "(set by your organisation)". New devices are asked for a code, also in the free edition. Delete the file and restart: back to the user's choice. |
| 14 | Free edition (trial over) with it off: choose "Require a code to connect (Pro)…". | The AirGlass Pro dialog: "Requiring a code to connect is part of AirGlass Pro…". If it was turned on during the trial, it stays on and can be turned off. |
| 15 | With it on, check the log and `airglass.log` after a few pairings. | The codes themselves never appear in the log. |
| 16 | Mac: mirror from a Mac (Control Centre → Screen Mirroring) with it on. | The Mac asks for the AirPlay code too; the right code connects. |
| 17 | Loopback tests: `AirGlass.exe --loopback` + `airglass_test stream …` with it on in `config.ini`. Then with `AIRGLASS_TEST_PIN=4821` set for AirGlass, `airglass_test pin 4821 --port 7010`. | The stream test needs no code and passes. The pin test prints `PASS pin`. |

## Recording with sound (Pro, <kbd>R</kbd>)

| # | Steps | Expected |
|---|---|---|
| 1 | Mirror from the iPhone, play a video with sound (or a YouTube clip with speech), press <kbd>R</kbd>. | Balloon "Recording the mirrored screen and its sound to Videos\AirGlass…". The log says `record: started …, sound 44100 Hz AAC` (or 48000). |
| 2 | Record about a minute, press <kbd>R</kbd>, play the MP4 (Films & TV, VLC, and drop it in a browser tab). | Picture and sound, lips in sync with speech from start to end. |
| 3 | Mirror with no sound playing, start recording, then after 10 s start a video with sound; stop after another 20 s. | The first 10 s are silent, then the sound comes in at the moment it started on the phone, in sync. |
| 4 | Pause the phone's video for 10 s mid-recording, then resume. | Silence for the pause, then sound in sync again (no drift after the gap). |
| 5 | Record for 15 minutes with speech. | Still in sync at the end (within about a frame or two). |
| 6 | Rotate the phone while recording. | The recording finishes by itself as before; its sound runs to the end of its picture. |
| 7 | Start mirroring and record straight away, before any sound has played on the phone (check the log: no `audio: … mirroring` line yet). After 10 s play a video with sound; stop after another 10 s. | Balloon "Recording the mirrored screen and its sound…". Log: `record: started …, sound 44100 Hz AAC (silent so far)`, then `record: sound started; it joins the recording in sync`. The MP4 is silent for 10 s, then has the sound, in sync with the picture (no "stays picture-only" line any more). |
| 7a | Record from a sender that never sends sound. | The MP4 has a picture and a silent sound track, as long as the picture. |
| 8 | Check the file in File Explorer → Properties → Details, or `ffprobe`. | Video H.264 at the sender's size; audio AAC stereo 160 kb/s. |
| 9 | Free edition: press <kbd>R</kbd>. | The AirGlass Pro dialog, as before. |
| 10 | Leave the phone on a still screen (home screen, nothing moving) for 2 minutes, press <kbd>R</kbd>, wait about 8 s without touching the phone, press <kbd>R</kbd>. | The MP4 is about 8 s long (not minutes) in Films & TV, VLC and a browser tab, and shows the home screen from the first instant to the end. Log: `record: finished (8.x s)`; the tray's "Stop recording (0:0x)" counted up while nothing moved. |
| 11 | Record a still screen for 5 s, then scroll for 5 s, then stop. | 10 s: the still picture, then the scrolling, at the moments they happened. |

## AirGlass Pro dialog

| # | Steps | Expected |
|---|---|---|
| 1 | Free edition: tray → **Buy AirGlass Pro ($9.99)…**. | Top line "Buy once, use it on up to 3 PCs." Below: "AirGlass Pro adds the floating glass window, screenshots, recording with sound, drawing, region zoom, OBS mode, and the allow-list and codes for new devices. One-time $9.99, no subscription." The whole text is visible (not cut off) at 100 %, 150 % and 200 % display scaling; the buttons and key box sit below it, nothing overlaps. |
| 2 | Press <kbd>D</kbd> (or <kbd>O</kbd>, <kbd>Z</kbd>, <kbd>S</kbd>, <kbd>R</kbd>) in the free edition. | The same dialog with the feature's own first line. |
| 3 | Paste a valid key, Activate. | "Thank you! AirGlass Pro is unlocked: the glass window and all the Pro tools are yours on this PC." |
| 4 | Fresh install (delete `trial=` under `[license]`), mirror for the first time. | Balloon "Your 7-day AirGlass Pro trial has started" naming T, S, R, D, Z and O. |

## Two devices at once (Pro)

You need two Apple devices (an iPhone and an iPad, two iPhones, or an iPhone and a Mac). Keep the log open (tray →
Open log folder → `airglass.log`).

| # | Steps | Expected |
|---|---|---|
| 1 | Pro or trial: mirror from the iPhone in the glass window. Then start mirroring from the iPad. | A second glass window opens **next to** the first (on the side with more room, smaller if it has to be), showing the iPad. The iPhone keeps mirroring. Log: `rtsp: connection … mirrors to window 2 (2 of 2 at once)` and `sessions: … in window 2; 2 mirroring at once`. |
| 2 | Play a video with sound on both devices. | Only the iPad's sound plays (the newest session). |
| 3 | Click the iPhone's window. | Its sound plays now and the iPad's stops, within a moment, without clicks or a mix of both. Log: `sessions: window 1 has the focus…` and `the sound now comes from connection …`. Change the volume on the iPad: nothing changes; on the iPhone: the PC follows. |
| 4 | Press <kbd>Tab</kbd> in that window. | The iPad's window comes to the front and its sound plays. <kbd>Tab</kbd> again goes back. |
| 5 | Right-click the tray icon. | "Show mirroring windows", a greyed "Tools for “<device used last>”" and "Switch to “<other device>”  Tab". Screenshot, recording, OBS mode, zoom and draw act on the window used last. |
| 6 | Press <kbd>S</kbd> in each window, and <kbd>R</kbd> in both (record a few seconds of video with sound on both), stop both. | Each PNG shows its own window; the second window's files end in " (2)". Each MP4 has its own device's picture **and its own sound**, even the one whose sound wasn't playing. |
| 7 | <kbd>D</kbd>, <kbd>Z</kbd>, <kbd>O</kbd>, <kbd>T</kbd> in one window. | Only that window draws, zooms, turns green or stays on top. In OBS mode the second window is titled "AirGlass (OBS 2)" (OBS can capture both). |
| 8 | Move, resize and rotate each device. | Each window behaves on its own; nothing flickers in the other. |
| 9 | Stop mirroring on the iPad. | Its window melts away; the iPhone keeps going and its sound plays. |
| 10 | With both mirroring, start a third device (or stop and restart mirroring on one of them, so it counts as new). | The **oldest** session is taken over: its window shows the new device; the other window is untouched. The oldest device shows mirroring has ended. Log: `takes over the oldest mirroring session`. |
| 11 | Close one window with ✕. | Only that device disconnects. |
| 12 | Play music (AirPlay audio, not mirroring) to the PC while two devices mirror. | Music plays as in 1.0.5. |
| 13 | Free edition (trial over, or loopback with `AIRGLASS_DEBUG_FREE=1`): mirror from the iPhone, then from the iPad. | As in 1.0.5: the iPad takes over full screen and the iPhone shows mirroring has ended. The AirGlass Pro dialog opens once: "Mirroring two devices at once is part of AirGlass Pro…". A later takeover in the same run doesn't ask again; the same device reconnecting never asks. |
| 14 | With *Ask before new devices connect* and/or *Require a code to connect* on, start the second device. | It is asked about / asked for a code as before; the first keeps mirroring meanwhile. |
| 15 | TV Mode connected: mirror from two devices. | Both windows float topmost without taking the focus. When the second opens on the first one's corner, the first slides to the other bottom (or top) corner. TV Mode's remote drives the newest window; when that one stops, it drives the other. |
| 16 | Loopback: `tools\two_devices.ps1`, then `tools\two_devices.ps1 -Free` (after `build.py` and the test media). | `PASS two devices (Pro)` and `PASS two devices (free edition: takeover)`. |

## Status over HTTP, keep-awake and full=1 (with two devices)

Add `[status] http=1` (and for row 5 `[window] full=1`) to `%LOCALAPPDATA%\AirGlass\config.ini` and restart AirGlass.
From another PC or phone on the LAN, open `http://<pc>:7000/status` ([details](status-http.md)).

| # | Steps | Expected |
|---|---|---|
| 1 | Nothing mirroring. | `"mirror":null,"sessions":[]`. |
| 2 | Mirror from the iPhone, then (Pro) from the iPad. | `mirror` is the iPad (same fields as before: `session`, `device`, `model`, `width`, `height`, `mode`); `sessions` lists both with `window` 1/2, and `recent:true` on the iPad. |
| 3 | `GET /status?since=<seq>` in one tab, then stop the iPad. | The request answers at once with `mirror` = the iPhone and one entry in `sessions`. |
| 4 | With both mirroring, set the PC's screen to turn off after 1 minute; wait 2 minutes. Stop one device and wait again. Stop both. | The display stays on while either device mirrors (log: `display: kept awake for mirroring`); it may sleep only after both stopped (`display: may sleep again`). |
| 5 | `[window] full=1`, Pro: mirror from two devices. | Both windows open full screen; <kbd>Esc</kbd> floats each one on its own. |

## Keyboard focus when a window opens

| # | Steps | Expected |
|---|---|---|
| 1 | Click the desktop (or any other app, e.g. Notepad), then start mirroring from the iPhone. | The glass window opens with the keyboard focus: press <kbd>S</kbd>, <kbd>D</kbd>, <kbd>Z</kbd>, <kbd>O</kbd> straight away, without clicking it, and each works. Nothing is typed into Notepad. Log: `ui: focus ok`. |
| 2 | Free edition (full screen): the same. | <kbd>Esc</kbd> and <kbd>S</kbd> (Pro dialog) work without a click. |
| 3 | Mirror, and while it is open start mirroring from the same iPhone again (or another device takes over the window). | The window doesn't grab the focus again; whatever you were typing into keeps it. |
| 4 | Pro, two devices: with the first window focused, start the second device. | The second window opens beside it but the first keeps the focus (and the sound). |
| 5 | TV Mode connected: start mirroring. | The window floats without taking the focus (no `ui: focus` line). |
| 6 | If row 1 fails: note the `ui: focus denied (the foreground window is "…")` line. | |

## Media controls (Pro, <kbd>Space</kbd>, <kbd>Ctrl</kbd>+arrows)

Keep the log open. Mirror from the iPhone and wait for `dacp: remote control available` (a few seconds).

| # | Steps | Expected |
|---|---|---|
| 1 | Play music or a video on the iPhone (in Music, Podcasts or YouTube) while mirroring. Press <kbd>Space</kbd> in the window. | It pauses on the phone; <kbd>Space</kbd> again plays. Log: `media: playpause (mirroring sender)` and `dacp: playpause -> 200`. Holding <kbd>Space</kbd> toggles once. |
| 2 | <kbd>Ctrl</kbd>+<kbd>→</kbd>, then <kbd>Ctrl</kbd>+<kbd>←</kbd>. | The next track, then back (`nextitem`, `previtem`). |
| 3 | <kbd>Ctrl</kbd>+<kbd>↑</kbd> a few times, then hold <kbd>Ctrl</kbd>+<kbd>↓</kbd>. | The phone's volume slider moves up a step each time, then down while held, and the PC's sound follows (**device test:** note whether the phone shows its volume HUD and whether the PC's level changes). |
| 4 | Right-click the tray icon. | **Media controls ▸** with "Play / pause  Space", "Next track  Ctrl+Right", "Previous track  Ctrl+Left", "Volume up  Ctrl+Up", "Volume down  Ctrl+Down"; each works. Before `dacp: remote control available` (or from a device without it) the item isn't there. |
| 5 | Draw mode on (<kbd>D</kbd>), press <kbd>Space</kbd>. | Play/pause as before; nothing is drawn or changed in the drawing. |
| 6 | Two devices: press <kbd>Space</kbd> in each window. | Each controls its own device. |
| 7 | A sender without remote control (log `dacp: the sender did not advertise a remote-control service`): press <kbd>Space</kbd>. | Balloon "No media controls" once per session; nothing else happens. |
| 8 | Free edition: press <kbd>Space</kbd> or <kbd>Ctrl</kbd>+<kbd>→</kbd>; tray → "Media controls (Pro)…". | The AirGlass Pro dialog: "Media controls for the device are part of AirGlass Pro…". |
| 9 | Plain arrow keys without <kbd>Ctrl</kbd>. | Nothing happens (as before). |
