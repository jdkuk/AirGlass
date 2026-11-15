# Status over HTTP

A read-only view of the current session for apps on the LAN that can't use the [control pipe](control-pipe.md), such as
a TV app that shows the PC's HDMI picture while it mirrors. It's served on the AirPlay port (TCP 7000), so it needs no
extra firewall rule.

It's **off** by default. To turn it on, add this to `%LOCALAPPDATA%\AirGlass\config.ini` and restart AirGlass:

```ini
[status]
http=1
```

Anyone on the local network can then see whether something is mirroring or playing, with the device name and track. It
can't control anything.

## `GET /status`

```json
{"seq":12,"v":1,"receiver":"Frame OS","screen":[3840,2160],
 "mirror":{"session":7,"device":"Taylor's iPhone","model":"iPhone17,1","width":886,"height":1920,"mode":"full"},
 "sessions":[{"session":7,"device":"Taylor's iPhone","model":"iPhone17,1","width":886,"height":1920,"mode":"full",
              "window":1,"recent":true}],
 "music":null}
```

* `mirror` is `null` when nothing is mirroring. With two devices mirroring at once (Pro), it is the most recent
  session, the one TV Mode follows; when that one ends, the other. `width`/`height` are the picture's shape (0 until the first frame,
  and portrait phone pictures arrive without their pillarbox bars). `mode` is where AirGlass draws it: `full` (aspect-fit
  on the whole `screen`, always so in the free edition), `float`, `hidden` or `free`.
* `sessions` lists every live mirroring session (`[]` when none), in window order, with the same fields as `mirror`
  plus `window` (1 or 2) and `recent` (true for the one in `mirror`).
* `music` is `null` unless audio-only AirPlay is playing:
  `{"session":9,"device":"…","title":"…","artist":"…","album":"…","durationMs":215000,"positionMs":12345,"rate":1,
  "artworkSeq":3,"codec":"ALAC","lossless":true}`. It updates about every 5 s while playing.
* Long-poll: `GET /status?since=12` waits until `seq` is no longer 12 (up to 25 s, or `&wait=<s>`), then answers.
  Without `since` it answers at once.
* `Access-Control-Allow-Origin: *`, `Cache-Control: no-store`.

## `GET /status/artwork`

The music session's cover art (`image/jpeg` or `image/png`), or 404. A new `artworkSeq` means new art.

## Full screen by default

On a PC whose screen is a TV, every session can open full screen (in Pro too, and in both windows when two devices
mirror; Esc still floats it):

```ini
[window]
full=1
```

## Display

While any mirroring session runs, AirGlass keeps the display and the PC awake, and wakes a display that had idled off
when the session starts. To turn either off:

```ini
[display]
keepawake=0
wake=0
```
