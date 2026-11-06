# Native export experiment

This branch adds a C99 native renderer and two video writers. Exporting needs
no ffmpeg. Building the specialized renderer needs the same C compiler and
make used to build the engine.

```sh
make native-export DEMO=demos/carnival.dmt
build/native/carnival demos/carnival.dmt build/carnival.mov --seconds 36 --size 960x600 --fps 30 --threads 8
```

Use `.mov` for lossless QuickTime Animation RGB24 video and stereo PCM audio.
Use `.avi` for uncompressed BGR24 video and stereo PCM. Both keep the requested
output dimensions and the demo's existing internal `song scale`. No additional
resolution reduction, frame skipping, palette reduction, or lossy encoding is
used. The executable also accepts `--scale N` for explicit preview experiments.

`make build/native/NAME` builds any demo in `demos/NAME.dmt`. The normal build
dependencies reuse the generated executable until the source or engine changes.
An executable refuses to render a demo whose bytecode differs from the one it
was compiled for. Palettes, samples, and textures are loaded from the demo as
usual. The experiment remains separate from `dm render`.

## How it works

The VM dispatches each opcode for each pixel. `tools/dm-aot.c` reads verified
module bytecode and emits straight-line C for frame, pixel, and post programs.
Structured branches and loops become C labels and jumps. The generated C
reuses the opcode bodies and math helpers from `src/dm_vm.c`, including guarded
division, secondary results, texture sampling, and feedback. The C compiler
can then remove stack operations, hold intermediate values in registers, and
fold constants. Audio continues to use the interpreter.

Uniform writes in pixel programs previously raced across workers. Each worker
now starts each pass with a private snapshot of frame uniforms. Writes remain
visible within that worker's pass and do not modify another worker or the
frame's uniforms. Carnival exposed this issue in the equivalence test.

The MOV writer uses horizontal RGB runs and skips pixels/rows unchanged from
the previous frame, with a keyframe every two seconds for seeking. This suits the enlarged pixels and static regions in many
demos. It preserves all RGB and PCM bytes; it does not achieve H.264's much
smaller files. Raw AVI conversion and PCM writes are also buffered in batches.

## Measurements

Measured on an M2 Max, Apple Clang 17, default `-O2`, at 960x600 and 30fps.
These are export times with a previously compiled native executable.

| Workload | Interpreter | Native | Export speedup |
| --- | ---: | ---: | ---: |
| Showcase, 3 seconds, raw AVI, 8 workers | 6.58s | 1.28s | 5.1x |
| Carnival, 1 second, lossless MOV, 1 worker | 39.23s | 3.62s | 10.8x |

Carnival's process CPU time fell from 37.35s to 3.54s (10.6x), supporting the
wall-clock result on a machine also running other tasks. Its native rebuild
took 0.81s with the engine objects already built. Include that one-time build
cost when measuring a short first export; subsequent exports reuse the binary.
Speedup varies with shader code and available CPU capacity.

The three-second showcase MOV was 75,563,358 bytes (72.1 MiB), versus
156,052,100 bytes (148.8 MiB) for raw AVI. The earlier H.264 MP4 was about
1.8 MiB. Compression alone is not the source of the rendering speedup.

Each export prints separate pixel, upscaling, audio, and encoding/write times.
The original two audio passes are retained because synthesizer output depends
on render chunk boundaries; simply reusing the visual replay would alter PCM.

## Verification and limits

```sh
make test
make test-native
```

The native suite compares complete output files against the interpreter for
every demo, then tests 44 seconds of showcase and carnival to cover scene
changes, loops, post processing, and feedback. The initial 42-demo run matched
byte-for-byte. ffmpeg, when installed, is used only to validate decoding in
tests. RGB and PCM decoded from the MOV writer were also compared exactly with
the uncompressed inputs.

Exact comparisons apply to the tested compiler and default flags. Avoid
`-ffast-math`; this compiler experiment does not promise cross-platform floating
point identity. The source extractor deliberately fails if it cannot find an
opcode body; VM formatting changes may require updating it.

These prototype containers use 32-bit offsets and are limited to files below
4 GiB. Raw AVI is checked against its full size before rendering; MOV must fit
within the actual compressed size limit. Playback has been validated by
ffprobe/ffmpeg decoding, not an exhaustive test of desktop players or browsers.
