# dmscene TODO — gaps found by dogfooding

Status: v4. Updated 2026-11-07 after native export integration, the supported
Wasm target and source metadata. Remaining experimental limitations are labelled separately.
This is a working list, not a roadmap promise. It records what actually hurt
while building 40+ demos, twelve model-authored demos, a four-model exquisite
corpse, and five attempted ports of the Pouet all-time top five.

**Priority legend**

- **P0** — blocks faithful recreation or safely handing the tool to an agent.
- **P1** — tooling and confidence; makes the next 100 demos cheaper.
- **P2** — expressive features; valuable but not blocking.
- **S / M / L / XL** — rough effort, not a schedule.
- **Impact** — how much it changes what demos can look like or how fast we can iterate.

Already landed in v4 and no longer gaps: structured `if`/`else`, P6 PPM
textures, `tex()`, `--render-scale`, `dm_seek`, branch/texture tests. Keep this
file honest: move completed capabilities to the closed section with their commit;
keep unfinished parts of a larger item open. See [CLI reporting](docs/cli-reporting.md)
and the [state-machine audit](docs/state-machines.md) for current behavior.

---

## P0 — fidelity and safe authoring

### 1. No mesh or textured-geometry pipeline
**Gap:** everything is a full-screen pixel/post program over SDFs, fractals or
2D math. There is no triangle rasterizer, no OBJ/glTF, no z-buffer, no UV
mapping on geometry, no normal maps, no material system.
**Evidence:** the top-five ports fail exactly here. `debris` needs a city of
buildings and cube swarms; `popular` needs a checkered room, columns and a
dancer with a textured body; `heaven7` needs raytraced spheres and a pagoda;
`lifeforce` needs a stone gear and a temple gateway; `elevated` needs a
displaced terrain mesh. SDF proxies read as flat symbols next to the originals.
**Suggestion:** do not start with a full 3D engine. Add a **2.5D textured layer**
first: screen-space quads with perspective, z-sorting, `tex()` sampling and a
small transform stack. That covers dancers, cards, UI, posters, sprites and
city-card parallax. If real meshes become necessary, add an optional tiny OBJ
loader + triangle rasterizer behind a compile-time flag; keep the core pure C99.
**Effort:** L (2.5D), XL (mesh). **Impact:** very high for fidelity.

### 2. No image import beyond P6 PPM
**Gap:** `texture` accepts only uncompressed PPM, max 1024×1024, 8 textures,
24 MiB total. Bilinear, clamped, no wrap/mipmaps. No PNG or JPEG input; the PNG
code is write-only.
**Evidence:** reference-driven work has to convert every screenshot by hand.
Rotoscoping, photo textures, posters and faithful colour matching are painful.
**Suggestion:** add a minimal PNG decoder (zlib inflate + filters + RGB/RGBA,
no third-party code) and `texture` support for PNG. Add wrap modes, a mip chain
and a `texlod()` variant. Keep PPM as the zero-dependency fallback.
**Effort:** M. **Impact:** high. **Acceptance:** load a 2048² PNG, sample with
mips and no shimmer.

### 3. 8-bit framebuffer and previous-frame buffer
**Gap:** the framebuffer and `prev` are 8-bit RGB. There is no HDR, exposure or
tonemap stage. Bloom is hand-rolled from `tap()`; feedback accumulates
quantisation and clips at 1.0.
**Evidence:** `voidzoom` loses detail after long recursive zoom; `cinder`,
`hollowstar` and `signal` clip on transients; every bloom is a compromise.
**Suggestion:** add an optional 16-bit float (or 16-bit half) scene/prev buffer
with a final tonemap/exposure pass. Keep the 8-bit path for speed. Start with
float `prev` only — feedback is where the artefacts are worst.
**Effort:** L. **Impact:** very high for light/feedback.

### 4. No arrays, indirect indexing or per-object state
**Gap:** 200 locals and 32 `$state` slots per program, no arrays, no structs, no
dynamic indexing. Particles, cube swarms, crowds, city blocks and per-object
animation must be analytic or unrolled by hand.
**Evidence:** `debris` cube swarms, `lifeforce` leaf clouds, `popular` crowds and
`elevated` terrain detail all fight this. Agents repeatedly unroll 8–16 copies
and hit code-size limits.
**Suggestion:** add fixed-size local arrays (`a[16]`) with constant or clamped
indices, and a small `for` over them. Alternatively add a per-frame "object list"
uniform buffer: `obj(i, field)` reading a compact table written by `frame`.
This is more useful than a general heap and keeps the verifier simple.
**Effort:** L. **Impact:** very high for scenes with repetition.

### 5. No camera, transform or keyframe system
**Gap:** no camera object, no projection matrix, no FOV helper, no keyframes,
no easing library, no object transforms. Every camera path is hand-written in
pixel space; every move is a bespoke expression.
**Evidence:** the ports need specific camera moves (debris pull-backs, elevated
ridge flight, popular cuts). Authors spend most of their time rebuilding camera
math instead of art direction.
**Suggestion:** add `cam` uniforms/helpers (position, target, fov, roll), an
`ease()`/`key()` pair driven by timeline events, and a `lookAt`/`project`
intrinsic for 2.5D. Even a `camera(t, k0..k3)` spline helper would remove a lot
of boilerplate.
**Effort:** M. **Impact:** high.

### 6. Text is a single 5×7 bitmap font with no layout
**Gap:** ASCII-only 5×7 glyphs in a 6×8 cell, 16 strings × 1024 chars. No
wrapping, alignment, kerning, safe-area helpers, custom fonts or UTF-8.
**Evidence:** `parable`'s moral was clipped by the bottom edge; `platform`'s
station sign rendered upside-down until fixed; `lastcall`'s board ran off the
right edge; `lifeforce`'s credit line clipped. Some model workflows could not inspect the rendered frame, so those
authors missed the overflow. This is not a limitation of every agent/tool setup.
**Suggestion:** add `text(s, x, y, size, align)`, `textw()`/`texth()`,
`textwrap()`, a safe-area rect, and a `check --strict` warning when a string's
bounding box leaves the frame. Later: import a bitmap font (PPM strip) and
basic UTF-8.
**Effort:** S/M. **Impact:** high for credits, lyrics, UI, posters.

### 7. Timeline semantics are order:row only, with traps
**Gap:** events are positioned by order:row, not seconds. Both `evt()` and
`evtl()` use the first event value before that event's position; `evtl()` then
interpolates between events. This is defined prefill behavior, but surprising
for a late-appearing name unless pinned to 0 at 0:00. `select()` is an instant scene switch; there is no crossfade. There
are no absolute-time events independent of the order list.
**Evidence:** the exquisite corpse hit this repeatedly: the first `1:00` fired
at 3 s because positions are order indices; `evtl` leaked; tick quantisation
put the next row 0 a few ms before the boundary. `signal` documented the same
leak.
**Suggestion:** allow `@seconds name=value` events alongside `O:R`, add
`evt0(name)` with an explicit default, and add `mixscene(a,b,k)` or a
`sceneBlend` uniform so scene changes can crossfade without hand-written
branching. Lint for `evtl` names whose first event is not at 0.
**Effort:** M. **Impact:** high for direction and correctness.

### 8. No render resume, frame ranges or chunked output
**Gap:** a render is all-or-nothing. `--start` and `--seconds` exist, but there
is no `--frames`, no segment/checkpoint, no tile render, no concat helper.
**Evidence:** 100 s `voidzoom` took ~7 min; 20 min under load for `nocturne`;
agents exceeded tool timeouts and left half-written MP4s (`chimaera` stage 2,
`popular`). A failure means starting over.
**Suggestion:** add `--frames A:B` and `--segment N/M` that write numbered
segments, plus `make concat` or `dm concat`. Add `--resume` for PNG sequences.
Write MP4 to a temporary sibling and rename on success so an interrupted
render does not replace a good output. Write/flush failures now return nonzero
and temporary WAV cleanup is fixed (`a1b8d3f`); atomic final MP4 replacement
and resume are still missing.
Native `--avi`/`--mov` now write without ffmpeg and atomically replace their
destination after finalization. MP4 keeps the existing ffmpeg path; the native
formats have larger files and a 4 GiB limit, so they do not replace resume.
**Effort:** M. **Impact:** high for iteration and long demos.

### 9. No lint, bench or cost model
**Gap:** `check` only compiles/verifies. It does not warn about silent tails,
clipping, unused instruments/patterns/text, unreachable scenes, text overflow,
feedback gain > 1, deep/expensive loops, or a predicted render time. `bench`
does not exist.
**Evidence:** agents ship demos that pass `check` but clip, go silent, or take
20 minutes. The top-five ports are expensive; we estimate cost only by running
them.
**Suggestion:** `dm check --strict` for static warnings; `dm bench` that renders
3 frames at low res, measures µs/pixel, and extrapolates to the requested
duration/resolution. Add `--budget SECONDS` that fails if the estimate is over.
Per-program bytecode and resource counts are now available through
`dm stats --json`; measured audio gates and render-cost estimates remain open.
**Effort:** S/M. **Impact:** very high for agent workflows.

### 10. No golden-image or visual regression tests
**Gap:** tests cover compile, round-trip, determinism, audio, errors and small
branch/texture/worker pixel assertions, but there are no committed representative
scene images. A shader change can still alter a demo without failing a test.
**Evidence:** we change `src/` (v4 branches/textures) and only find visual
regressions by rendering and looking. `platform`'s upside-down sign and
`lifeforce`'s flat temple were caught by eye, not tests.
**Suggestion:** commit 3 golden PNGs per demo (start/mid/end, small size) and
compare in CI, with an explicit update path. Exact hashes are suitable only
for a pinned build, platform, thread count and render history. Cross-platform
libm differences and worker-local shader state require tolerances or separate
baselines; determinism tests do not promise universal pixel identity. Start
with a few representative scenes at 320×200 instead of 3 images for every demo.
**Effort:** S/M. **Impact:** high.

### 11. Sampler and audio limitations
**Gap:** embedded samples are 8-bit mono, loaded from 8/16-bit PCM WAV.
Import keeps the source rate unless `rate=N` is specified (stored rate is
capped at 65535 Hz); playback output is 44.1 kHz. No loop points, root note, stereo samples, velocity layers,
multisampling or round-robin. No MIDI import/export, no audio input, no FFT,
no convolution reverb, no compressor/limiter/chorus/phaser built-ins (only
`dly`, `verb`, `svf`, `dist` and hand-built effects). 32 voices, 8 channels,
4 delays/4 reverbs per program, 2 s max delay.
**Evidence:** the ports need specific instruments and mixes; `popular` cannot
use the original vocal hook; `lifeforce` needed a bigger percussive arc;
`elevated` needed an orchestral build. Everything is hand-synthesised.
**Suggestion:** add loop points/root note/velocity layers to `sample`; allow
stereo samples or two mono samples; add a `midi` import (text or SMF) for
composition; add a limiter/compressor and a chorus/phaser primitive. Audio
input/FFT is a larger, optional track.
**Effort:** M/L. **Impact:** high for music fidelity.

### 12. Language limits: no `break`/`continue`, 4-deep loops, no recursion
**Gap:** no early exit from loops (mask with `step()`), no `continue`, loops
nest 4 deep, fns inline (no recursion), `&&`/`||` are non-short-circuit. Active repeat
counters are now read-only, and matching loop/end operands are verified
(`a1b8d3f`); nested products can still be expensive.
**Evidence:** complex per-pixel logic becomes nested `if`s and masks; agents
hit loop-depth limits and duplicate code.
**Suggestion:** add `break`/`continue` with verifier stack checks, raise loop
depth to 8, and add short-circuit boolean variants (`and`, `or`) if the VM can
do it without changing stack semantics. Keep recursion out; fns inline is a
feature for size/speed.
**Effort:** M. **Impact:** medium-high for authoring.

---

## P1 — tooling and confidence

### 13. Structured diagnostic locations and symbol names (remaining)
**Delivered:** `dm check --json`, `dm stats`, and `dm stats --json` now report
validation status, resource counts/asset bytes, per-program bytecode/locals/state/
effects, and pixel scene indices/names. They accept source or binary modules.
**Remaining:** diagnostics preserve the compiler's message but do not expose
separate line/column/code fields or multiple errors. DMS v4 does not retain
uniform names, so stats reports their count rather than a named symbol table.
**Suggestion:** add structured source locations at the compiler error source;
consider optional symbol metadata if named inspection of binaries is useful.
**Effort:** S/M. **Impact:** medium for tooling; basic machine-readable output is done.

### 14. Demo manifest, gallery and contact sheets
**Delivered:** static gallery/detail pages, posters, source highlighting and
live Wasm/Studio playback, with `make web` / `make site` / `make serve`.
Optional `.dmt` header metadata supplies credits, descriptions, custom tags
and duration; existing tracked-demo credits were migrated from README tables.
Per-demo JSON is derived output; no separate editorial manifest is required.
**Remaining:** measured render/audio metrics, contact sheets and optional
generation of README rows from the source metadata.
**Suggestion:** extend the generated reports with measured metrics; retain
source comments as the editable metadata. Generate README rows/contact sheets
from those reports rather than adding a second hand-maintained manifest.
**Effort:** M. **Impact:** high for sharing and comparing.

### 15. Reference/fidelity harness
**Gap:** no way to compare a render to a reference frame or to score fidelity.
We did it by eye, one screenshot at a time.
**Suggestion:** `dm compare render.png reference.png` with side-by-side,
perceptual hash/SSIM and a contact sheet. A `--reference` mode for ports that
records which shot maps to which original frame. Add a fidelity column to the
manifest (faithful/approximate/inspired).
**Effort:** M. **Impact:** high for ports and visual QA.

### 16. Coverage-guided fuzzing and execution budgets (remaining)
**Delivered:** macOS/Linux CI runs `make test-sanitize` with ASan, UBSan and
float-cast-overflow checks. Mutation/regression tests cover malformed structured
branches and loops, texture data, numeric lookup indices, retry ownership and
seek cancellation. Loop-counter mutation and invalid ENDL operands are rejected
(`a1b8d3f`). See `tests/branch-texture.c`, `tests/state-safety.c`,
`tests/vm-numeric.c`, and [the audit](docs/state-machines.md).
**Remaining:** no coverage-guided fuzz harness/corpus and no per-frame
instruction/time budget. Legal nested loop counts can still burn substantial time;
fixed allocation/error cases are not exhaustive fuzzing.
**Suggestion:** fuzz targets for source and binary loading, with corpus retention
and bounded execution; add a runtime budget without breaking valid expensive demos.
**Effort:** M. **Impact:** high for robustness.

### 17. Cross-platform CI and native Windows audio
**Gap:** CI covers macOS and Linux. Windows is untested; the non-Apple audio
path is a pipe to `aplay`, not a native backend.
**Suggestion:** add a Windows build job (MSVC/Clang-cl or MinGW) and a WASAPI
backend, or document Windows as unsupported. Add a deterministic cross-platform
golden test.
**Effort:** M/L. **Impact:** medium.

### 18. Browser playback limits (remaining)
**Delivered:** `make wasm` / `make web`, pinned Emscripten CI, a dedicated
worker using the real compiler/VM/tracker/synth, Web Audio and canvas playback.
Tests compile every demo and exercise synthesis, pixels, retry and nested assets.
**Remaining:** audio is prepared up front (capped at 300 seconds); rendering
uses one worker at preview resolution. Streaming synthesis, longer playback,
and additional performance work remain optional follow-ups. See [Wasm](docs/wasm.md).
**Effort:** M/L for streaming. **Impact:** high for long demos.

### 19. LSP, formatter, syntax highlighting and a tutorial
**Gap:** `.dmt` has no editor/LSP integration, formatter, parser grammar package
or step-by-step tutorial. The gallery has source highlighting, which
is distinct from editor support.
The README is an excellent reference but a poor first lesson.
**Suggestion:** a `tree-sitter-dmt` grammar + highlight queries, `dm fmt`, and a
`docs/tutorial.md` that builds a demo from `dm new` to a finished piece. Add a
shader/SDF/feedback cookbook.
**Effort:** M. **Impact:** high for humans and agents.

### 20. Debug overlay and uniform inspector
**Gap:** no way to see uniforms, current scene, frame time, per-pass cost or the
value under the cursor. `play` already shows title, time, beat, estimated FPS,
transport state/speed, channel mute flags and reload status; it is not a
uniform or per-pass profiler.
**Suggestion:** `--debug` overlay with uniforms, scene index, frame time, worker
count; a `dm inspect` mode that prints the uniform table. Optional per-pixel
probe at the cursor.
**Effort:** M. **Impact:** medium-high.

### 21. Hot reload state and live uniform editing
**Gap:** `watch` resets uniforms and feedback on successful reload; there is
no live uniform editing. Playback already has pause, speed, timeline scrubbing
and channel muting. Failed reloads now retain the active module, precise file
stamps catch rapid saves, and failed retries release their allocations (`a1b8d3f`).
**Suggestion:** preserve uniforms/feedback across reloads when the uniform
layout is unchanged; add keyboard/OSC tweaking for uniforms. Timeline
scrubbing is already implemented (`d15847a`, `ab790e4`). This would make direction sessions much faster.
**Effort:** M. **Impact:** high for authoring.

### 22. Seed control and reproducibility
**Gap:** `hash()`/`noise()` are deterministic but there is no explicit seed.
There is no user-selectable RNG seed for reproducible variations. Fixed
per-voice/worker RNG state already exists; it does not guarantee bit-identical
floating-point output across operating systems or thread counts.
**Suggestion:** `--seed N`, `song seed=N`, and per-program RNG streams; document
which intrinsics are seeded. Add a cross-platform float determinism test.
**Effort:** S/M. **Impact:** medium.

### 23. Install and release packaging (remaining)
**Delivered:** `dm --version` / `-V` reports the v4 engine generation and DMS
format version without a banner or additional output.
**Remaining:** no install target, Homebrew formula or prebuilt release binaries.
An independently versioned application release/build identifier is also distinct
from the current DMS format number.
**Suggestion:** `make install` with `PREFIX`/`DESTDIR`, then tagged GitHub
releases with macOS/Linux binaries; define release versioning before adding a
separate `VERSION` file or package formula.
**Effort:** S/M. **Impact:** medium.

### 24. Raymarch/SDF helper library
**Gap:** every demo redefines `sdBox`, `sdSphere`, `sdSegment`, soft shadows,
AO, normals, repetition.
**Suggestion:** built-in SDF primitives and operators (`sdBox`, `sdSphere`,
`sdTorus`, `smin`, `repeat`, `normal`, `softshadow`, `ao`, `envmap`) plus a
`docs/cookbook` page. This is a small VM surface change with a big authoring
win.
**Effort:** M. **Impact:** high.

### 25. Scene compositor: crossfade, layers, multiple targets
**Gap:** one pixel program per frame, one `post`, one `prev`/`cur`. Scene
changes are instant; layers must be blended by hand.
**Suggestion:** `mixscene(a,b,k)`, optional `layer` programs with alpha, and a
second render target (ping-pong) for feedback/blur. Start with scene crossfade;
it removes a common hack.
**Effort:** L. **Impact:** high for direction.

### 26. Colour management
**Gap:** 8-bit sRGB-ish math everywhere; no linear workflow, gamma control,
LUTs or tonemapping. Highlights clip abruptly.
**Suggestion:** optional linear-light pixel math + a final gamma/tonemap, or at
minimum an `exposure()`/`tonemap()` post helper and documented colour space.
**Effort:** M. **Impact:** medium-high.

### 27. Alpha, transparency and layers
**Gap:** no alpha channel, no transparency, no compositing. Overlays are
additive or masks only.
**Suggestion:** an alpha component in the accumulator and a `blend(a,b,alpha)`;
or a separate `sprite` pass with alpha. Needed for text, cards, dancers and
posters.
**Effort:** M. **Impact:** medium-high.

### 28. Time signatures, polyrhythm and groove
**Gap:** `bar = beat/4` assumes 4/4. One global row clock; no per-channel
pattern lengths, no time-signature changes, no swing templates beyond `Fxx`
speed changes.
**Suggestion:** `song meter=3/4` or per-pattern meter; a `swing` directive;
per-channel clock dividers. Keep the tracker model; just allow the grid to
change.
**Effort:** L. **Impact:** medium.

### 29. Audio effects: dynamics and modulation
**Gap:** no compressor/limiter, chorus, phaser, flanger, EQ, convolution or
sidechain. `dly`/`verb`/`svf`/`dist` cover a lot but not everything.
**Suggestion:** add a limiter (protects the master), a chorus/phaser, a
3-band EQ, and a sidechain input to `svf`/gain. These are small DSP additions
with a big mix-quality payoff.
**Effort:** M. **Impact:** high for music.

### 30. Live input and sync
**Gap:** no MIDI/OSC input, no audio input, no external clock, no live
parameter control beyond the keyboard.
**Suggestion:** MIDI-in for note/CC control and clock; OSC for uniforms; audio
input + envelope follower. Useful for liveacts and installations.
**Effort:** L. **Impact:** medium.

---

## P2 — larger expressive bets

### 31. GPU backend
**Gap:** CPU-only, scanline threads. A 100 s 960×600 render is minutes; complex
scenes are 20+ minutes.
**Suggestion:** a Metal/Vulkan/WebGPU backend for the pixel/post programs, with
the VM/compiler unchanged. Treat as an optional accelerator, not a rewrite.
**Effort:** XL. **Impact:** very high performance, but a major architectural
bet. Keep CPU as the reference path.

### 32. Full triangle rasterizer / OBJ / glTF
**Gap:** no mesh path at all.
**Suggestion:** only if 2.5D textured quads + SDF prove insufficient. A tiny
OBJ loader + z-buffered triangle rasterizer with UVs and a simple material is
enough for demoscene-scale scenes; skip glTF/PBR initially.
**Effort:** XL. **Impact:** high for fidelity, high risk.

### 33. PBR materials, normal maps, environment maps
**Gap:** shading is ad hoc; no normal maps, no env maps, no roughness/metalness.
**Suggestion:** after the 2.5D/texture path, add a simple matcap/env-map sampler
and normal-map perturbation. Do not build a full PBR pipeline unless meshes land.
**Effort:** L. **Impact:** medium-high.

### 34. Path tracing / global illumination
**Gap:** no GI, no path tracing, no BVH.
**Suggestion:** probably not. A small SDF path tracer for stills/short shots
could be a fun sidecar, but it fights the real-time/demo ethos. Defer.
**Effort:** XL. **Impact:** low for the core.

### 35. Video import / rotoscoping
**Gap:** no video decode, no frame sequence import, no rotoscope source.
**Suggestion:** allow a directory of PPM/PNG frames as a texture sequence, with
`frame`-indexed sampling. This is enough for rotoscoped silhouettes without a
video decoder.
**Effort:** M. **Impact:** high for ports/narrative.

### 36. Vector/bezier/SVG
**Gap:** no curves, no SVG, no vector shapes; everything is math or glyphs.
**Suggestion:** `bezier(p0..p3,t)`, `path()` and a tiny SVG path parser for
logos/credits. Useful for titles and UI.
**Effort:** M. **Impact:** medium.

### 37. Sprite system
**Gap:** no sprites, no billboards, no 2D object list.
**Suggestion:** `sprite(tex, x, y, w, h, rot, alpha)` in a dedicated pass, sorted
by z. This overlaps with the 2.5D layer; build them together.
**Effort:** M. **Impact:** medium-high.

### 38. Physics and collision
**Gap:** no physics; debris/crowds/particles are faked.
**Suggestion:** not a general physics engine. A tiny verlet/particle integrator
with fixed arrays would cover most demo needs. Depends on arrays (#4).
**Effort:** M. **Impact:** medium.

### 39. Networking and multi-user
**Gap:** none. Not needed for the core.
**Suggestion:** skip unless liveacts/multiplayer become a goal.
**Effort:** XL. **Impact:** low.

### 40. Plugin system
**Gap:** no extensions; every intrinsic is compiled in.
**Suggestion:** a stable intrinsic table already exists (`dm_ops.h` X-macro).
Expose optional intrinsic modules at build time rather than a runtime plugin ABI.
**Effort:** M. **Impact:** low-medium.

---

## Deliberate constraints (trade-offs, not bugs)

These are part of the project's identity. Revisit only with a clear reason.

- **No third-party libraries.** Keeps the build trivial and the code auditable.
  A PNG decoder, font importer or mesh loader must be written in-tree.
- **C99 core, one binary, no linked third-party libraries.** Keep it that way.
  MP4 export currently needs ffmpeg; the non-Apple audio backend needs an
  external player (default `aplay`). The optional gallery generator uses Node
  and ffmpeg/ffprobe. These are existing tool dependencies, not a package manager.
- **Text-first `.dmt`.** The DSL is the editor. Do not replace it with a GUI;
  add tooling around it.
- **CPU-first.** GPU is an optional accelerator, not the reference path.
- **Embedded assets.** Modules are self-contained; keep it that way.
- **8-bit output.** Fine for delivery; the gap is the intermediate buffers
  (#3), not the final MP4.
- **No general-purpose 3D engine.** The 2.5D/texture path is the cheaper bet
  for fidelity; a full rasterizer is a deliberate escalation.

---

## Dogfooding evidence

### The five Pouet top-five ports
- `debris` — city scale, cube swarms, folding streets, giant ring, sky fracture.
  Blocked by no mesh/instancing (#1, #4), 8-bit feedback (#3).
- `elevated` — photoreal snow/rock/water flythrough, sun flare, motion blur.
  Blocked by no terrain primitive (#1), no HDR (#3), CPU cost (#31).
- `heaven7` — raytraced spheres, reflections, light corridor, magenta sphere.
  Blocked by no mesh/ray primitives (#1), no env maps (#33).
- `lifeforce` — temple, cenote, stone gear, heart-skull emblem, polaroid,
  sunset tower. Blocked by no image import/rotoscope (#2, #35), text layout
  (#6), and the model's inability to see frames (a model/tooling gap, #15).
- `popular` — checkered disco room, textured dancer, chrome ring, mirror ball.
  Blocked by no textured geometry (#1), alpha (#27), and the free model limit
  (not an engine issue).

### 40+ authored demos
- `voidzoom` — recursive feedback detail collapse → 8-bit `prev` (#3).
- `chimaera` — timeline index/quantisation traps, `evtl` leak → #7; 24-row
  pattern timing surprise (3 s, not 12 s) → lint/tutorial (#9, #19).
- `parable`, `platform`, `lastcall`, `lifeforce` — clipped/flipped/overscanned
  text → text layout + strict warnings (#6, #9).
- `morrow` 7.5 min, `signal` 11–15 min, `nocturne` 20 min under load →
  bench/resume (#8, #9, #31).
- `cinder`, `hollowstar` — transient clipping → limiter (#29), HDR (#3).
- Agent-authored demos — silent tails, clipping, expensive loops, unused
  resources → `check --strict` + `bench` (#9).
- `popular` / `lifeforce` / `cinder` — agents cannot see images in some
  models, so visual verification is on us → fidelity harness (#15), not an
  engine gap.

---

## Next small wins, ranked after this review

1. Static authoring warnings: unused patterns/resources and timeline names with
   a nonzero first event position. Keep measured audio checks separate.
2. `dm bench`: sampled frame timing with explicit resolution/thread/history
   settings and a clearly approximate estimate. Audio, scene changes and feedback
   mean three frames cannot establish a reliable whole-demo budget.
3. `make install` with `PREFIX`/`DESTDIR` and a staged-install smoke test.
4. Atomic MP4 output replacement. This is smaller than resume/frame ranges/concat
   and protects an existing successful export from a failed retry.
5. A small representative visual regression corpus with platform-aware comparisons.

Already implemented by this review: `dm stats`, `dm stats --json`,
`dm check --json`, and `dm --version` (see closed section).

The old quick-win list understated PNG decoding + mipmaps, float feedback,
text layout/lint, seed plumbing, cameras and arrays. Those remain useful work,
but are engine/format changes or several features bundled together. Keep them
as separately scoped follow-ups rather than one afternoon's cleanup.

---


## Closed / delivered capabilities

Larger numbered items above retain only their unfinished scope.

| Capability | Commit / evidence |
|---|---|
| Structured branches and embedded P6 textures | `26b238f`; branch/texture regression tests |
| Render-scale override and encoder failure reporting | `fe490ba` |
| macOS/Linux CI | `7d9caf8`; `.github/workflows/ci.yml` |
| Safe terminal input, pause/speed/channel controls | `4a13d5b` |
| Asynchronous scrubbing and prompt input wakeup | `d15847a`, `ab790e4` |
| Sanitizer CI, loop verifier hardening, numeric lookup guards | `a1b8d3f`; `make test-sanitize` |
| Failed reload ownership, rapid-save detection, writer error propagation | `a1b8d3f` |
| JSON validation, module/program statistics, version flag | `74e37da`; [reporting contract](docs/cli-reporting.md) |
| Native AVI/MOV flags and optional AOT shader builds | [Native export](docs/native-export.md); lossless decode/failure regressions |
| Supported Emscripten target and browser playback | `make wasm`, `make test-wasm`; [browser engine](docs/wasm.md) |
| Source-owned demo metadata and gallery | `make test-web`; [metadata fields](docs/demo-metadata.md) |
| Kitty/Ghostty images, capability detection, adaptive preview, native shader playback | [Terminal graphics](docs/terminal-graphics.md); protocol, scrubbing, feedback-resize and AOT fallback tests |

Native AVI/MOV export is integrated behind explicit flags. Shader AOT remains
optional per-demo build tooling; its speedups do not describe the ordinary
interpreted MP4 exporter. GPU rendering remains open (#31).

---

## Open questions (answer with more dogfooding)

- Is the goal **fidelity** or **expression**? Fidelity argues for #1/#2/#5;
  expression argues for #3/#4/#25.
- Is CPU-only a hard constraint? If yes, #8 and #9 are mandatory for long demos.
- What is the render budget per demo — 5 min, 20 min, an hour?
- Is PPM enough, or do we need PNG/JPEG? (Almost certainly PNG.)
- Are 8-bit intermediate buffers acceptable for feedback-heavy work? (No.)
- Do we need real meshes, or can 2.5D quads + textures carry the look?
- Is Windows/browser distribution a goal?
- What does "close recreation" mean numerically? Adopt the rubric below.

## Fidelity rubric

Use this in `demos/manifest.json` and the README for ports.

- **Faithful** — shot order, composition, palette and key silhouettes are
  recognizable; no major motif missing; simplified geometry is acceptable only
  if the silhouette reads.
- **Approximate** — all key motifs appear, but geometry, framing or detail is
  visibly simplified; reads as the same scene at a glance.
- **Inspired** — mood, palette or a single motif only; not a port.
- **Miss** — the intended composition is not recognizable.

Do not call an *Approximate* a *Faithful* in the README. The point of the list
is to make the next pass cheaper, not to grade on a curve.
