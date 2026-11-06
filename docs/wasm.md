# Browser engine and build targets

The browser player uses the same C compiler, verifier, sequencer, synth and
scene renderer as `dm`. Emscripten compiles them to WebAssembly. JavaScript
loads assets, manages playback and draws the returned RGB frames; it does not
implement the demo VM or synthesizer.

## Build and run

Install Node.js 22+ and the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html).
The tested SDK release is recorded in [.emscripten-version](../.emscripten-version).
From your SDK checkout, install and activate that version, then source
`emsdk_env.sh`. Alternatively set `EMCC` to the full path of its `emcc` executable.

From the dmscene repository:

```sh
make wasm                  # compile the engine only
make web                   # compile engine, then generate the static gallery
make serve                 # http://127.0.0.1:8000/
make test-web              # metadata, source display and site generation
make test-wasm             # actual Wasm: every demo, audio, frames, retry and assets
# EMCC=/path/to/emsdk/upstream/emscripten/emcc make web
```

`make web-runtime` remains an alias for `make wasm`. `make site` generates
pages using an existing runtime, if present; it does not require Emscripten.
Ordinary `make` and `make test` remain native builds without a Node/SDK requirement.
CI separately builds and tests the pinned Wasm toolchain.

Objects and dependency hashes live in `build/wasm/`, separate from native
objects. Changing C sources, shared headers, build flags or the compiler
version invalidates the relevant cached build. The output pair is
`web/runtime/engine.mjs` and `engine.wasm`. A failed compilation leaves the
previous output pair in place. `make clean-wasm` removes the cache and runtime.
The generated site/runtime are build artifacts; the source is in `web/_sitegen/`
and `web/_runtime/`.

## Runtime boundary

The ES module exports an asynchronous factory, usable in a browser module
worker or Node. The browser worker fetches one `.dmt` and its declared assets,
writes the assets into Emscripten's in-memory filesystem, and calls `web_open`.
Relative subdirectories are preserved: `assets/a/image.ppm` and
`assets/b/image.ppm` remain distinct resources.

The small C adapter in `web/_runtime/engine.c` exposes:

| Entry point | Contract |
|---|---|
| `web_open(source)` | Compile/verify source and initialize the demo; zero means success |
| `web_error()` | Last compiler/runtime error string |
| `web_audio(frames)` | Render 1..1470 interleaved stereo float frames at 44.1 kHz |
| `web_env()`, `web_order/row/tick()` | Synth levels and tracker position |
| `web_frame(seconds,w,h,env,reset)` | Render RGB; reset reconstructs the tracker clock and clears visual history |
| `web_width/height()` | Actual internal resolution after the demo's `scale` |
| `web_close()` | Free the current demo and its owned state |

Returned buffers belong to the engine. Copy them before the next operation
that reuses them, and read fresh heap views after calls that may grow Wasm
memory. The worker transfers copies of complete frame/audio buffers to the UI.

This target defines `DM_NO_THREADS`, explicitly selecting one scene worker.
It does not depend on pthread stubs, SharedArrayBuffer, COOP or COEP headers.
All expensive compilation, synthesis and pixel rendering happen in a dedicated
worker. See [Emscripten modular output](https://emscripten.org/docs/compiling/Modularized-Output.html).

## Playback and static hosting

Live modes synthesize the selected soundtrack before playback, then use a
Web Audio buffer as the clock while the worker renders frames on demand.
This makes seeks cheap after preparation, at the cost of startup time and
memory. The current player caps live preparation at 300 seconds and targets
30 fps. Studio requests 160×100 pixels; terminal mode requests 80×50 pixels,
displayed as the equivalent of 80×25 half-block cells. Demo `scale` can reduce
the internal resolution further. Seeking restarts feedback; it does not replay
all earlier images. Shader cost and browser scheduling can reduce frame rate.

Serve `web/` over HTTP(S), with `.wasm` as `application/wasm` and `.mjs` as
JavaScript. `make serve` already does this. There is no server-side demo engine
and no need for an MP4 for live playback. MP4 remains an independent video mode.
ffmpeg/ffprobe are optional for site generation: without them, source/live
pages still build; MP4 probing/posters are omitted.

Source credits and descriptions come from [metadata comments](demo-metadata.md).
The site's JS source scanner is for highlighting and inspection. Its static
timeline timestamps are estimates; tracker tempo/jump/loop effects can change
actual timing. Live tracker positions come from the C sequencer.
