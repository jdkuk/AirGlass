# Control pipe API

Another app on the same PC can **place and drive the AirGlass window** and **control AirPlay music** over a named pipe.
It was built for a living-room "TV Mode" shell that's driven with a remote: AirGlass floats over whatever is on screen
and never steals keyboard focus.

* Pipe: `\\.\pipe\AirGlass.Control` (loopback test instance: `\\.\pipe\AirGlass.Control.Loopback`)
* UTF-8 JSON, **one object per line**, in both directions. Several clients may connect.
* Security: the pipe's DACL allows only the current user, and remote clients are rejected.
* A client that stops reading is dropped after 500 ms.

While at least one client is connected the window is **TV-controlled**: topmost, `WS_EX_NOACTIVATE`, placed by
`layout`, with a 4-button capsule (full · corner · size · stop). When the last client disconnects, AirGlass waits 5 s
(so a client restart doesn't flicker) and then returns to normal desktop behaviour.

> [!NOTE]
> In the **free edition**, sessions are full screen. A `layout` that asks for `float` opens the upgrade prompt instead of
> leaving full screen.

## AirGlass → client (events)

On connect, AirGlass sends `state`:

```json
{"event":"state","version":1,"receiver":"Living Room PC","session":0,"device":"","model":"","width":0,"height":0,
 "mode":"float","corner":"br","size":"s","visible":false,"nowplaying":null}
```

| Event | Example |
|---|---|
| `started` | `{"event":"started","session":7,"device":"Taylor's iPhone","model":"iPhone17,1","mode":"float","corner":"br","size":"s"}` |
| `size` | `{"event":"size","session":7,"width":886,"height":1920}`: first frame, and on every rotation |
| `layoutChanged` | `{"event":"layoutChanged","session":7,"mode":"float","corner":"br","size":"s","rect":[x,y,w,h],"by":"tv"}`. `mode` is `float`, `full`, `hidden` or `free` (moved or resized with the mouse). `by` is `tv` or `user`. |
| `userClosed` | `{"event":"userClosed","session":7}`: the user pressed ✕ / Stop |
| `stopped` | `{"event":"stopped","session":7,"reason":"sender"}`, where `reason` is `sender`, `user`, `tv` or `takeover` |
| `nowplaying` | audio-only sessions; see below |
| `mediaResult` | `{"event":"mediaResult","rid":"m3","ok":true}` or `{…,"ok":false,"error":"no-remote"}` |

`nowplaying` (on every change plus a 5 s heartbeat; `{"event":"nowplaying","session":9,"active":false}` when it ends):

```json
{"event":"nowplaying","session":9,"active":true,"device":"…","title":"…","artist":"…","album":"…",
 "durationMs":215000,"positionMs":12345,"rate":1,"artworkSeq":3,"artworkType":"image/jpeg",
 "artworkPath":"C:\\Users\\you\\AppData\\Local\\AirGlass\\nowplaying.jpg",
 "volume":0.8,"controls":true,"codec":"ALAC","sampleRate":44100,"bitDepth":16,"lossless":true}
```

Artwork is written atomically, so a new `artworkSeq` means a new file.

## Client → AirGlass (commands)

| Command | Effect |
|---|---|
| `{"cmd":"state"}` | re-send `state` |
| `{"cmd":"defaults","mode":"float","corner":"br","size":"s","margin":48}` | layout for the *next* session (`mode`: `float`, `full` or `hidden`) |
| `{"cmd":"layout","mode":"full","corner":"tl","size":"m","margin":48}` | layout of the running session |
| `{"cmd":"highlight","button":"corner"}` | focus a capsule button (`full`, `corner`, `size`, `stop`; `null` hides the capsule) |
| `{"cmd":"press","button":"corner"}` | press animation (the client then sends the resulting `layout`/`stop`) |
| `{"cmd":"stop"}` | end the mirroring session |
| `{"cmd":"media","action":"playpause","rid":"m3"}` | `play`, `pause`, `playpause`, `next`, `prev`, or `volume` with `"value":0..1` |

Geometry is AirGlass's job. Sizes `s`, `m` and `l` are equal-area: a 16:9 picture gets 28 / 40 / 55 % of the screen width,
and other shapes get the same area. Corners are `br`, `bl`, `tr` and `tl`, and the corner is kept when the device rotates.

## Minimal client (Node.js)

```js
const net = require('net');
const pipe = net.connect('\\\\.\\pipe\\AirGlass.Control');
let buf = '';
pipe.on('data', d => {
  buf += d;
  for (let i; (i = buf.indexOf('\n')) >= 0; buf = buf.slice(i + 1)) {
    const ev = JSON.parse(buf.slice(0, i));
    console.log(ev);
    if (ev.event === 'started') pipe.write(JSON.stringify({cmd: 'layout', mode: 'float', corner: 'tr', size: 'm'}) + '\n');
  }
});
```
