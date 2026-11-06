# dmscene showcase site

The static site in `web/` is generated from `demos/*.dmt` and `build/video/*.mp4`.
Everything website-related lives in this folder: the generator, the CSS/JS
sources, the self-hosted font, and the local preview server.

```sh
make web                    # build Wasm and generate web/ (from repository root)
make videos                 # optional: render MP4s for video mode
make site                   # regenerate pages with the existing runtime
make serve                  # preview at http://127.0.0.1:8000/
```

`sitegen.mjs` scans the demos, reads video metadata with `ffprobe`, extracts
poster frames with `ffmpeg`, copies videos/sources/assets into `web/`, and writes
plain HTML. `dmt.mjs` scans source structure and provides syntax highlighting;
`metadata.mjs` reads optional header comments. The output is static HTML with
JavaScript for filtering, video controls and the live Wasm player. ffmpeg and
ffprobe are optional: without them, source/live pages still build.

## Font

[Obel Mono](https://typographic.systems/obel-mono) by Typographic Systems,
self-hosted from `assets/fonts/` under the SIL Open Font License 1.1. The license
and author files are copied into `web/fonts/` with the WOFF2 files.

The featured slot selects the rendered demo with the most parsed timeline events
(ties sort by filename). Gallery counts, filters and source details are derived
from the current demos and videos. Credits, descriptions, tags and intended
preview lengths come from [demo metadata comments](../../docs/demo-metadata.md).
The generator does not read README tables or require an editorial manifest.
`demos.json` supplies the Random link on pages without gallery cards; in the
gallery, Random respects the active filters.

## Live terminal and Studio

Demo pages offer Video, Live terminal and Studio when `web/runtime/engine.wasm`
is present. The latter two compile the original `.dmt` in a dedicated worker,
using the actual C compiler, sequencer, synth and scene renderer. Samples and
textures are loaded into Emscripten's in-memory filesystem. Audio is synthesized
once on entering a live mode, with progress shown, then played through Web Audio.
Only the selected demo is loaded. Rendering stays in the worker; hiding the tab
pauses playback.

Terminal mode uses the same truecolour top/bottom pixel mapping as
`dm_term_blit` (80 × 25 cells), painted to a canvas rather than sending ANSI
escapes through a terminal emulator. Studio renders at 160 × 100 and adds the
tracker, score clips, timeline events and program source. Score clips are built
from the sequencer's actual order transitions during synthesis, including loops
and tempo changes. Selecting a score clip seeks and opens its pattern; selecting
a program opens its code. This is an inspection player, not a source editor.
Feedback visuals reset after a seek, as in native interactive playback.

Rebuild the browser engine after changing C code:

```sh
# Install/activate Emscripten through https://emscripten.org/docs/getting_started/downloads.html
make wasm                   # or EMCC=/path/to/emcc make wasm
make web                    # includes wasm + site
make test-web
make test-wasm
```

The generated `runtime/engine.mjs` and `runtime/engine.wasm` are self-contained
static assets. The site needs no backend, terminal service, React, CDN, or
cross-origin-isolation headers. Serve `.wasm` as `application/wasm` for streaming
compilation (the preview server does this). MP4 playback remains available if
WebAssembly, Workers or Web Audio cannot start. Live modes may take several
seconds to prepare a complex soundtrack; they never download or use the MP4.

Source views use self-hosted Obel Mono, preserve spaces and tabs, and never wrap
tracker rows. Code wrapping applies to other code. The source tests compare every
highlighted demo line with the original, including blank-line whitespace.

`make test-wasm` smoke-tests the actual Wasm artifact
against every demo (including samples/textures), PCM synthesis, rendering, rewind,
and recovery after an invalid source. See [Browser engine](../../docs/wasm.md)
for the pinned SDK, build cache, runtime interface and deployment limits.

Studio occupies the full browser viewport in a native dialog, with a fixed
project header, transport, program browser, monitor, code inspector, tracker and
bottom arrangement. On narrow screens, Monitor / Tracker / Code / Programs tabs
switch the upper pane while the arrangement stays available below. Drag its
separator (or use Up/Down with the separator focused) to resize the arrangement.
Zoom changes the horizontal time scale; drag the ruler to scrub. The master
waveform is calculated from the synthesized stereo audio, not illustrative data.
The active pattern and original code row follow playback unless Follow is off.
Space plays/pauses, Left/Right seek five seconds, Home restarts, and Escape exits.

Opening Studio updates the URL to `?mode=studio`. Direct links and refresh open
Studio; browser Back/Forward restore the selected mode. `?mode=terminal` is also
supported. Source, download and gallery actions remain standard links.
