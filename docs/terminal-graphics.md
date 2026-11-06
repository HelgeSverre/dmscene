# High-resolution terminal playback

`dm play` can send its RGB framebuffer directly through the Kitty graphics
protocol. Ghostty and Kitty support it; other terminals retain ANSI truecolor
half blocks (one column and two vertical pixels per cell). There is no
Ghostty-specific renderer or new demo format.

```sh
./dm play demos/hakkuh.dmt                         # probe, then images or text
./dm play demos/hakkuh.dmt --graphics text         # force half blocks
./dm play demos/hakkuh.dmt --graphics kitty --size 640x400
make play-native DEMO=demos/hakkuh.dmt             # compile shaders, then play
make play-native DEMO=demos/hakkuh.dmt PLAY_ARGS='--size 640x400 --fps 30'
```

`make native-player DEMO=demos/hakkuh.dmt` builds without launching. Its output,
`build/native-player/hakkuh`, accepts the normal `dm` commands and flags.
Generated shaders are bound only to matching bytecode; loading or watching a
changed/different demo falls back to the interpreter. Rebuild to specialize
the edited demo. Audio remains interpreted. These targets need a C compiler
and a demo under `demos/NAME.dmt`; ordinary playback needs neither a compiler
nor an external image/video encoder.

## Selection and resolution

`--graphics auto` is the default. After entering raw mode, it sends a tiny
nonblocking `a=q,t=d,f=24,s=1,v=1` image query with a dedicated image id.
Only a complete matching `ESC_Gi=ID;OK ESC\\` APC reply within one second
selects images. Playback and input continue in text mode while probing.
Redirected descriptors do not probe. `--graphics text` skips detection;
`--graphics kitty` forces the protocol for a connection known to support it.
Neither `TERM` nor `TERM_PROGRAM` is treated as proof of capability. A
multiplexer or slow connection can block the reply even in Ghostty; auto then
keeps text output. No multiplexer-specific passthrough is implemented.

Image placement fills the terminal viewport above one reserved status row.
`TIOCGWINSZ` supplies physical dimensions, or an 8x16 pixel-cell estimate is
used when pixel fields are absent. The initial framebuffer fits within
640x400, retaining the viewport aspect ratio. The demo's `song scale` still
applies; `--render-scale N` overrides it, with 1 meaning full detail.

Without `--size`, Kitty playback measures render plus output time and adjusts
resolution toward `--fps` (30 by default), reserving 20% for other work. It
reduces detail promptly after sustained overload and restores it gradually,
with hysteresis to avoid oscillation. It never drops below one quarter of the
initial width/height. This is a quality/performance tradeoff, not a guarantee
of frame rate under arbitrary CPU or network load. Feedback is resampled on
resize so resolution changes preserve trails. The status line shows the
actual internal dimensions.

`--size WxH` fixes the image dimensions before demo scale and disables adaptive
resolution. Use it for predictable quality and comparisons. In text mode it
retains the existing column/half-block sizing behavior.

## Frame transport and lifecycle

Frames are 24-bit RGB sent through direct transport (`a=T,t=d,f=24`), with
explicit image dimensions and cell placement (`c,r,C=1`). An internal base64
encoder emits at most 4096 payload characters per chunk. Only the first chunk
carries image metadata; subsequent chunks carry the continuation flag. All
chunks for a frame finish before another graphics command. The encoder reuses
one buffer and writes the assembled frame, avoiding a syscall per chunk.
Normal image commands use `q=2` to suppress acknowledgements.

Two image ids alternate: transmit/display the new frame, then delete the old
image and its storage using `a=d,d=I,i=OLD`. This avoids replacing a currently
visible id halfway through transmission and bounds terminal image storage.
The image/status update is wrapped in synchronized output (`CSI ?2026h/l`).
Resize and reload replace the old placement on the next complete frame.
Quit, handled output failure and suspension clean up both owned ids and leave
the alternate screen; resume reopens and reprobes. Unrelated images are never
deleted. Abrupt process termination cannot guarantee cleanup.

The input parser captures only a bounded APC reply while swallowing arbitrary
APC/OSC/DCS payloads. Embedded `q`, arrows or scroll text cannot become commands.
Normal keys remain in the input queue; probing does not consume them separately.

## Measurements and next optimizations

A 640x400 RGB frame is 768,000 bytes, about 1.02 MB after base64, or roughly
30.7 MB/s at 30 fps before command overhead. Higher resolution also multiplies
shader work. A local M2 Max probe rendering 24 Hakkuh frames, excluding the
first four, measured:

| Path at 640x400, automatic worker count | Render time/frame | Base64 + write to `/dev/null` |
|---|---:|---:|
| Interpreter | 108.02 ms | 0.39 ms |
| Compiled shaders | 15.36 ms | 0.35 ms |

That is about 7x less render time with the same dimensions. The output timing
excludes terminal decoding/display and is not a Ghostty frame-rate claim.
Other demo renders were active during this work, so these are directional
measurements. Native compilation provides a speedup without lowering resolution;
adaptive playback trades detail for responsiveness when rendering is too slow.

Shared memory, files, compression and GPU shader execution remain future
options. File/shared-memory transport needs a separate successful capability
query and a shared namespace; it cannot be assumed over SSH. The first
implementation uses direct transport on every connection.

## Verification and sources

`tests/term-graphics.c` checks a pseudo-terminal query/reply exchange, a late
reply, malformed strings, ordinary queued keys, redirected-output fallback,
independent base64/RGB decoding, chunk boundaries, alternating ids and cleanup.
`tests/scrub-live.c` exercises real Kitty-mode playback through a pipe, including
scroll bursts, reversal, pause/resume, speed changes and quit. Frame-budget and
feedback-resize regressions live in the transport and scene tests. The native
suite checks specialized/fallback RGB and PCM, plus failed-rebind invalidation.
Visual inspection in Ghostty was unavailable through the computer-use tool;
manual checks of display, resizing and suspend/resume remain useful.

Primary references: [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
defines detection, transport, placement, ids and deletion; [Ghostty features](https://ghostty.org/docs/features)
confirms protocol support; [Ghostty synchronized output](https://ghostty.org/docs/help/synchronized-output)
describes atomic redraws. Ghostty's [1.4.0 release notes](https://ghostty.org/docs/install/release-notes/1-4-0)
describe its image protocol conformance work.
