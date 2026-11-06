# dmscene state machines and failure audit

Audit baseline: `ab790e4` (2026-11-06). Scope: the C99 application in `tools/`,
`src/`, and `include/`, its command-line entry points and platform backends.
The native video/AOT exporter in `experiment/native-video-export` is a separate,
unmerged experiment and is outside this inventory. States below describe the
current implementation; the findings retain the baseline's failing traces.

This is a code-and-regression audit, not a proof of all possible executions.
“Confirmed” means a reproduction or an explicit executable transition in the
code, not merely a suspicious field. P1 means termination, nontermination,
memory corruption, or false-success data loss; P2 means lost edits or resource
growth during normal recovery. Issue numbers link to the grouped fixes.

## Findings and resolution

| Priority / issue | Reachable invalid transition at baseline | Evidence and regression | Status |
|---|---|---|---|
| P1 [#1](https://github.com/HelgeSverre/dmscene/issues/1): counted loops | Verified loop → body overwrites counter → ENDL never reaches count; malformed ENDL can use an out-of-range/different local | `tests/state-safety.c`; baseline accepts all four invalid cases. `parse_repeat`, `parse_statement`, `dm_prog_verify`, VM `ENDL` | Fixed; active counters are immutable and unique while nested; ENDL must match LOOP |
| P1 [#2](https://github.com/HelgeSverre/dmscene/issues/2): audio lifecycle | Running pipe → child exits → process dies with SIGPIPE; closing → blocked writer/child → never returns. Worker and UI also access `stop`, `written`, `t0` without synchronization | `tests/audio-pipe.c`; baseline signal 13 for `exit 7`, deadline signal 14 for both sleeping-player cases. `pump`, `dm_audio_close`, `dm_audio_played` | Fixed; nonblocking cancellable writes, synchronized state, owned child group, bounded termination, failure notification and wall-clock fallback |
| P1 [#3](https://github.com/HelgeSverre/dmscene/issues/3): output commit | Buffered output → disk error during flush → reported success; WAV can continue counting unwritten samples | `tests/output-errors.c`; real `RLIMIT_FSIZE=1` failures, not mocked I/O. Baseline PNG, PPM, DMS and WAV CLI all report success | Fixed; write/close errors propagate, WAV errors are sticky and RIFF size is bounded |
| P1 [#5](https://github.com/HelgeSverre/dmscene/issues/5): numeric lookup boundary | Verified shader → arithmetic overflow → NaN → undefined integer conversion / invalid lookup index | `tests/vm-numeric.c`; UBSan aborts at baseline `pal_lookup` line 99 for `pal(p, pow(10,1000))` | Fixed; bounds/finite checks precede integer conversions, with defined fallback values |
| P2 [#4](https://github.com/HelgeSverre/dmscene/issues/4): reload/retry | Save A → save B in same second → no reload. Compile failure → longjmp bypasses temporary frees → repeated failures retain memory | `tests/watch-state.c`, `tests/compile-retry.c`; baseline equal timestamps and retained compiler allocations across retries | Fixed; exact timestamp/identity comparison, transactional candidate load, compiler-owned cleanup across longjmp |

### Reproduction details

1. **Loop safety.** `pixel { repeat i 2 { i = -1 } }`, with statements on
   separate lines, compiles at baseline. Each ENDL adds one to -1 and branches
   because 0 < 2. A nested `repeat i` can similarly reset an outer counter.
   Editing a valid module's ENDL operand to 255 passes verification although
   `locals` has only 200 entries. Verification now rejects all these paths,
   including through the binary loader. Valid nested loops, sequential counter
   reuse, and count-zero skipping remain supported.
2. **Audio.** Build the actual pipe backend with `DM_AUDIO_PIPE` on either OS.
   `DM_AUDIO_CMD='exit 7'` closes its reader while the pump writes. A command
   that never reads fills the pipe, so baseline `close` cannot join the writer;
   a command ignoring TERM also tests forced child cleanup. The test places a
   three-second deadline around each child. Shared fields are now read/written
   under one mutex; the callback is outside it. `close` joins before freeing.
   A separate terminal regression closes stdout's reader before opening the
   terminal: baseline dies with SIGPIPE; the fix requests orderly quit and
   restores the caller's signal handlers during close.
3. **Output.** A forked child ignores SIGXFSZ and limits regular files to one
   byte. Small PNG/PPM/DMS writes enter stdio buffers successfully and fail at
   close. Baseline ignores that result. The WAV CLI similarly prints
   `rendered 0.01s` and exits zero. Tests require failure status, including at
   the CLI boundary. This fixes reporting, not atomic replacement of an old
   output: a failed destination can contain partial data.
   The same export issue also covers descriptor ownership: starting the CLI
   with stdin closed makes `pipe` reuse descriptor 0. Baseline duplicated 0 to
   itself and then closed it; ffmpeg failed with `Bad file descriptor`. The
   child now closes the original descriptor only when it differs from stdin;
   `tests/run.sh` includes an actual MP4 export with `0<&-`. Temporary audio
   files use unique owned names and are removed on both success and failure.
4. **Retry.** `futimens` gives two saves the same seconds but different
   nanoseconds; baseline `mtime` discards the difference. Compiler allocation
   tracking observes leaked current blocks and function bodies when parsing
   fails; unterminated patterns and missing assets exercise the same ownership
   rule. A heap-owned compiler context survives `longjmp` without relying on
   modified automatic variables. `load_any` now owns a separate candidate and
   disposes it on failure, preserving the old module until success.
5. **Numeric boundary.** A valid compiled palette call with `pow(10,1000)`
   overflows to infinity; taking its fractional part produces NaN. UBSan
   reports `nan is outside the range of representable values of type int`
   before the palette index is used. The regression covers palette, sample,
   wavetable, glyph, string, accumulator lookup, fractal iterations and HSV.
   Palette/hue use zero for nonfinite coordinates, sample reads return zero,
   other indices are bounded before conversion and rejected/clamped by their
   lookup. Ordinary finite input retains its existing behavior.

## Ownership and concurrency map

| Actor | Owns / mutates | Shared contract |
|---|---|---|
| CLI/UI thread | module slots, scene, tracker preview, framebuffer, terminal parser, transport flags, reload stamp | A module is immutable while any player/scene/seeker refers to it |
| Audio callback / pipe pump | live player while inside `audio_ctx.mu`; backend counters inside backend mutex | UI pauses or swaps the player under the same mutex; never frees it during callback execution |
| Seek worker | private player, deep checkpoints, scratch synthesis | Requests/results/env/failure/stop under `seek.mu`; no terminal or device operations |
| Scene workers | disjoint framebuffer rows, private VM/inputs/uniform copy, persistent state per worker/pass | All workers join before post processing, history publication, resize, or module destruction |
| Encoder/player child | its input stream and external codec/device state | Parent owns pipe and child lifecycle; EOF is not evidence of child success |

Compiler/module errors use a process-global string. The CLI serializes these
operations. Concurrent compilation through the library is **not** supported.
Closing an object concurrently with an arbitrary API call on that same handle
is not supported; the UI is the lifecycle owner.

## 1. CLI and module transaction

Code: `tools/dm.c:main`, `cmd_compile`, `load_any`; `src/dm_module.c`.
States: argument parsing, candidate allocated, reading/compiling, verified,
active, failed, freed. `load_any` is the replacement boundary; lower-level
compile/load functions require an empty destination and do not replace a live
module safely by themselves. `dm_module_free` releases/nulls all owned arrays;
remaining scalar counts do not make that object executable.

| From | Event / guard | Action → next state |
|---|---|---|
| Parsing | valid command/options | choose command → candidate |
| Parsing | missing/invalid argument | usage/nonzero exit → finished |
| Candidate | `.dmt` / `.dms` | read and compile / parse chunks → verification |
| Candidate / verification | I/O, parse, allocation, verifier failure | free candidate, retain prior destination → failed |
| Verification | all checks pass | free old destination, transfer candidate ownership → active |
| Active | command completes / replacement commits | stop dependents, free module → freed |

Invariants: no unverified bytecode executes; a failed `load_any` cannot mutate
its destination; freeing a module requires its workers and callbacks to have
stopped. CLI error exits may leave partial output files; success requires
writer finalization. `new` is exclusive-create, not overwrite.

The DMS reader's nested machine is header → chunk tag/varint → bounded payload
→ next chunk → verify. Unknown tags skip their payload. Invalid version,
overflowing varint, truncation or invalid resource references go to error.
Partial allocations belong to the candidate and must be freed even on error.
Texture chunks additionally require ascending unique IDs, valid dimensions and
the exact RGB byte count. This format does not promise rejection of every
noncanonical encoding (for example repeated legacy chunks).

## 2. Compiler and scope

Code: `src/dm_compile.c:dm_compile`, `read_block`, `compile_program`,
`parse_statement`, `parse_if`, `parse_repeat`, `parse_call`.
State is the source cursor/line, function/name tables, current program kind,
expression stack depth, local high-water mark, state/delay/reverb counts and
the active structured blocks. Function expansion is bounded, not runtime
recursion. Programs have different permitted input/opcode sets.

| From | Input | Action → next state |
|---|---|---|
| Pre-scan | pixel/timeline names | register forward references → top-level parse |
| Top level | scalar/resource directive | parse/store resource → top level |
| Top level | program/function opener | own growing body buffer → collecting |
| Collecting | nested braces / outer closing brace | adjust nesting / finish body → compile or store function |
| Program | expression / assignment / call | emit stack-balanced instruction sequence → program |
| Program | if/else/repeat | enter block scope, patch jump offsets, merge locals → program |
| Any parse stage | `fail` | longjmp to owning context, free block/path/functions/module allocations → failed |
| End of source | successful verification | free compiler temporaries, transfer module → complete |

Invariants: statement stack depth returns to zero; branch-local declarations
do not become definitely assigned outside the branch; repeat counts are
integer constants 0..65535; active repeat locals cannot be assigned or reused
by nested repeats. Resource ownership is registered before a later parse can
fail. A retry starts from a new context; it never resumes a failed parser.

## 3. Bytecode verifier and VM control flow

Code: `src/dm_vm.c:dm_prog_verify`, `dm_vm_run`; `include/dm_ops.h`.
Verifier state: PC, operand depth, loop stack (start/end/depth/branch nesting/
counter slot), branch stack (targets/depth/loop nesting/else seen).
Runtime state: PC, operand stack, loop counts, locals, output registers and
explicitly bound persistent state. Bytecode is executed only after verification.

| From | Opcode / guard | Transition |
|---|---|---|
| Linear | ordinary opcode with valid immediate/resource and sufficient operands | advance PC, apply stack effect |
| Linear | LOOP | validate unused active counter; push structural loop; runtime zeroes local and either enters or skips count-zero body |
| Loop body | ENDL with matching slot, offsets, nesting and depth | runtime increments counter and jumps back or pops loop |
| Linear / block | IFZ | consume condition; push merge contract; runtime enter then or jump |
| Then | ELSE at declared target and equal depth | record end target; runtime skip else after then |
| Branch | ENDIF at correct target/depth/nesting | pop merge contract → enclosing block |
| Any | invalid opcode/operand, crossing blocks, stack under/overflow | verification failure; never execute |
| End | empty loop/branch stacks and operand depth zero | verified / runtime returned |

Every backwards edge is an ENDL of a counted loop; its counter cannot change
in its body. Nesting depth and each count are bounded, but the **product** of
nested counts can still be expensive; this is not a per-frame time budget.
The verifier checks structural memory indices, not that all floating-point
arithmetic remains finite. Runtime lookup boundaries now handle nonfinite or
out-of-range coordinates before converting to integers. This does not promise
that every intermediate shader/DSP value is finite.

## 4. Sequencer and timeline

Code: `src/dm_seq.c`; state: `started`, sample/tick position, BPM/speed,
order/row/tick, pending next position, loop counters and channel effects.

| From | Event | Action → next state |
|---|---|---|
| Initialized, first tick due | player/seek asks for samples | process first row/tick → running |
| Inside tick | advance ≤ remaining samples | increment sample/tick position → inside tick or boundary |
| Tick boundary | tick < speed | increment tick/beat, delayed note/cut/retrigger/effects → next tick |
| Row boundary | pending jump/break/loop | consume pending position once → enter target row |
| Row boundary | no pending jump | next row/order; end wraps to module loop → process row |
| Running | Fxx tempo/speed | recompute subsequent tick timing → running |
| Running | forward seek | repeatedly process ticks and partial span → target sample |
| Running | backward UI seek | initialize a fresh sequencer, then forward seek → target sample |

Invariants: order and row resolve to an existing pattern; advancing never skips
an unprocessed tick; pending order/row are consumed together. `dm_seq_seek` is
forward-only, not a reversible operation. Note-on increments a serial; holding
a note does not. Timeline hold/lerp values derive from arrangement position,
which can jump/loop; before the first event, the implementation uses that first
event's value. That prefill is existing behavior, not a confirmed bug.

## 5. Synth voices and effect storage

Code: `src/dm_synth.c`; voice identity is `(slot, channel, trigger serial)`.
States: inactive, held (`active && rel < 0`), releasing, retired/stolen.

| From | Event | Transition |
|---|---|---|
| Inactive / channel has no current voice | new trigger serial | allocate free slot or steal preferred victim → held |
| Held | new note, NNA=cut | retire old voice, allocate replacement → held |
| Held | new note, NNA=continue | old voice → releasing; new voice → held |
| Held | gate off | set release origin → releasing |
| Releasing | quiet threshold or release timeout | inactive |
| Any slot | stolen by another channel/serial | old channel reference is invalidated before use |
| Player initialized | render block | tick sequencer at boundaries; render voices/channel effects/master; clamp output |
| Player partial/full allocation | free | release all delay/reverb/mix buffers → freed |

Invariants: a channel cannot update a stolen voice through its old slot; unchanged
serials do not retrigger; gate controls release independently of volume. Audio
samples and envelopes depend on synth call boundaries for some programs, so
checkpoint/replay must preserve the 1024-frame source-block convention.

## 6. Scene frame, passes and feedback

Code: `src/dm_scene.c`; states: initialized, frame program, pixel workers,
post workers, history published, resized/reset, freed.

| From | Event | Transition |
|---|---|---|
| Initialized / previous frame | render request | bind clock/env/uniforms → frame program |
| Frame program | valid scene selection | choose pixel program; ensure history dimensions → pixel pass |
| Pixel pass | worker creation succeeds/fails | execute assigned rows on worker/calling thread; join → post or publish |
| Pixel complete | post program exists | snapshot current image, run/join post workers → publish |
| Publish | all passes complete | copy RGB to previous-frame buffer, increment frame → ready |
| Ready | seek/module replacement | clear scene/feedback and initialize new timeline origin → initialized |
| Ready | framebuffer resize | allocate matching feedback → render |

Invariants: workers write disjoint rows; each owns its VM and uniform copy;
post sees a completed immutable current image; previous-frame feedback is not
modified until all passes finish. Persistent pixel state belongs to a worker,
so changing worker counts can change stateful shader results. Full bitwise
equivalence across arbitrary thread counts is not promised. `dm_fb_alloc`
preserves its old allocation on allocation failure.

## 7. Asynchronous seek and cache

Code: `src/dm_seek.c`; request identity is `(generation, target, mute mask,
prepare_audio)`. Worker states: idle, replaying, copying, publishing, failed,
stopping. Result state: absent, ready for current generation, stale, taken.

| From | Event / guard | Transition |
|---|---|---|
| Idle / working | changed request | increment generation and wake → replay newest target |
| Working | identical request | coalesce; do not restart |
| Replaying | target behind worker | restore closest checkpoint ≤ aligned target, or fresh player |
| Replaying | mute mask changes | discard incompatible cache, start fresh player |
| Replaying | after each 1024-sample block | check stop/generation; cancel stale work or continue |
| Aligned target | prepare requested | deep-copy player, render final partial block only on copy → publishing |
| Publishing | generation still current and not stopping | swap ready result/env under mutex → idle |
| Publishing | superseded / stopping | destroy result → next request / stopping |
| Ready | take with matching generation | transfer ownership exactly once → absent |
| Any worker state | required allocation fails | publish failed flag → terminated |
| Any | close | set stop/wake, join, free cache/result → closed |

Invariants: worker player stays block-aligned; no published pointer aliases
mutable delay/reverb buffers; consumers cannot take stale results. Optional
cache allocation failure drops the checkpoint, not the request. Required
allocation failure is terminal; the UI exits rather than retrying a dead worker.
Cache is capped by count and byte budget. Closing joins before module free.

Repeated `take` without a new request returns null intentionally. Repeating a
consumed identical prepared request does not create a second result; current
UI consumption immediately switches to an environment-only request, so a
subsequent prepare changes request identity. No application trace violating
that contract was confirmed in this audit.

## 8. Playback transport and gestures

Code: `tools/dm.c:play_realtime`, `audio_cb`, `accept_audio`;
`src/dm_seek.c:dm_scrub_add/dm_scrub_tick`.
This is a product of flags, not one enum: paused, waiting for seek, active
gesture, audio available, speed index. Paused+waiting and scrub+waiting are
valid. The source timeline (`base`, `pos`) differs from device output samples.

| Event | Guard / action | Result |
|---|---|---|
| Normal timer | !paused, !waiting, !scrub | advance from device clock or wall clock, scaled by speed |
| Wheel burst | coalesce signed deltas; start/reset gesture if needed | hold audio; set velocity-sensitive continuous target; background seek |
| Gesture tick | active | interpolate displayed position toward target; draw without replaying audio on UI |
| Gesture settles | idle ≥120 ms and error <1 ms | stop gesture; wait for prepared audio or resume wall clock |
| Direction reversal | active | discard residual target/density, apply new direction immediately |
| Left/right | ±5 seconds, clamp lower bound | cancel gesture, reset scene, request new position |
| Up/down / space | speed within table / toggle pause | hold audio, prepare exact position before resuming |
| Current seek result | waiting and !scrub | swap player under audio mutex, reset resampler/device origin → ready |
| Channel toggle | any transport state | update live mute under mutex and replace pending seek if necessary |
| Audio failure | backend failed | close backend, preserve displayed position, resume wall-clock transport |
| q / quit signal | any | terminal/device/worker/player/module teardown |

Invariants: held callbacks output zeros without advancing synth state;
`accept_audio` resets interpolation/source buffers; waiting never uses an old
player at a new position; backward visual seeks reinitialize the tracker.
Scene history is cleared when seeking, not per wheel report. Drawing is capped
to coalesce bursts; the terminal wait wakes on input. Ghostty delivers discrete
wheel reports, not raw macOS pixel deltas; event density estimates gesture speed.

## 9. Reload and suspend transactions

Code: `tools/dm.c:mtime`, `same_stamp`, `load_any`, reload/suspend branches.

| From | Event | Transition |
|---|---|---|
| Playing / paused | watched fingerprint changes or `r` | parse independent candidate |
| Candidate | invalid source/binary | free candidate, display error, keep active player/scene/module |
| Candidate | valid module | allocate next seeker/player before replacing active state |
| Replacement ready | commit | hold/close old audio, join old seeker, swap player, free old scene/module, initialize new scene/clock |
| Reload committed | audio opens / fails | prepare retained timeline position / use wall clock |
| Playing / paused / seeking | Ctrl-Z | hold/close audio, restore terminal, raise default SIGTSTP → suspended |
| Suspended | SIGCONT | reopen terminal/audio; request current position → prior paused/playing intent |

The watch fingerprint compares existence, device, inode, size, seconds and
nanoseconds; it is not a floating-point timestamp. Failed versions are not
recompiled every frame: another edit or explicit `r` retries. Deliberately
rewriting a file while preserving every fingerprint field is not detected.
Audio/seeker callbacks must stop before the old module is freed. A required
allocation failure during reload exits cleanly rather than half-committing.

## 10. Terminal protocol and signal ownership

Code: `src/dm_term.c`; parser states are explicitly enumerated.

| State | Input | Next / output |
|---|---|---|
| TEXT | supported key / ESC / unknown | action / ESC / ignore |
| ESC | `[` / `O` / OSC,DCS,etc / intermediate | CSI / SS3 / STRING / ESC_INTERMEDIATE |
| CSI | digits/separators/private marker | collect bounded parameters, remain CSI |
| CSI | final byte | decode arrows, mouse, paste start if valid; otherwise swallow → TEXT |
| X10 | three mouse payload bytes, possibly fragmented | decode wheel only → TEXT |
| STRING / STRING_ESC | string terminator | swallow full payload → TEXT |
| PASTE | exact bracketed-paste terminator | swallow contents, including q/Ctrl sequences → TEXT |
| Any parser state | incomplete read | preserve state until remaining bytes arrive |

Terminal lifecycle: allocated → saved settings/handlers → raw mode and terminal
features enabled → input/blit/wait → restore settings/handlers → freed. stdin
EOF stops polling that descriptor; the application may still run until quit or
its time limit. SIGINT/TERM/HUP/QUIT/PIPE set quit, SIGTSTP requests the UI's suspend
transaction. The parser never treats an arbitrary escape-sequence byte as q.
Raw mode retains ISIG, so Ctrl-C/Ctrl-Z retain normal terminal semantics.

## 11. Audio backend lifecycle

Code: `src/dm_audio.c`. macOS: allocate AudioQueue → allocate/prefill three
buffers → start → callbacks refill/requeue → stop/dispose. Failure to create,
allocate or start unwinds and returns null. Playback position is AudioQueue's
sample clock. Device interruption/error reporting beyond these startup checks
is not modeled by the current macOS API wrapper.

Pipe backend:

| From | Event | Transition |
|---|---|---|
| Allocating | mutex/pipe/spawn/thread failure | close owned resources/child → null handle |
| Ready | worker callback completes | convert PCM, nonblocking writes → writing |
| Writing | pipe temporarily full | poll ≤20 ms, check cancellation, retry |
| Writing | full block accepted | update written count/start time under mutex → next block |
| Writing | permanent error / broken reader | set failed under mutex → worker terminated |
| Ready/writing/failed | close | set stop, join writer, close pipe, terminate/reap owned process group → closed |

Invariants: clock never exceeds fully submitted frames; counter snapshots are
synchronized; callback/user lifetime ends only after close returns. SIGPIPE is
blocked in the writer, with per-descriptor suppression on Darwin. The child
gets a separate process group; shutdown escalates TERM to KILL after roughly
200 ms. This bounds waits on normal user-space children, not an OS process
stuck in uninterruptible kernel I/O or an arbitrary callback that never returns.
Invalid negative/nonfinite configured latency uses the default.

## 12. Offline export and writer finalization

Code: `tools/dm.c:render_offline/start_ffmpeg`, `src/dm_io.c`,
`src/dm_module.c:dm_module_write`.

| From | Event | Transition |
|---|---|---|
| Initialized | start offset | synthesize discarded prefix → audio pass |
| Audio pass | block rendered | write WAV; any failure → failed |
| WAV complete | final header/flush/close succeeds | audio committed → video pass |
| MP4 requested | ffmpeg absent | PNG directory + WAV fallback; print reconstruction command |
| MP4 requested | ffmpeg present | spawn argv-based child, raw RGB pipe → video pass |
| Video pass | next frame | advance independent visual-audio player and sequencer, run scene, write PNG/pipe |
| Video pass | write/render failure | stop feeding; close/reap child → failed |
| Last video frame | close pipe and wait child | require all writes and zero child exit → complete |
| Any finalization | error | nonzero command status; never print success |

WAV substates: closed → opened/header → streaming → finalized/closed, with a
sticky failed state. Failed writes do not increment the frame count; further
writes fail; close always releases the stream and retains failure status.
RIFF32's byte limit is checked before export and while writing. PNG/PPM/DMS
success includes `fclose`, since stdio can defer an error until then.
An output error is not automatically retried: retrying append writes would risk
duplicate/corrupt data; the user reruns the export from a fresh writer.

Audio and visuals use separate players because visual envelopes are replayed
after the audio pass. Encoder input is an argument vector (paths are not shell
code). The fallback command shell-quotes filenames. Encoder exit status is
checked even if every `fwrite` succeeded. The existing ffmpeg path can still
block if an encoder never reads/exits; this audit's bounded-child change is
currently limited to realtime audio.

## Verification and limits

The six new regression programs are part of `make test`, alongside existing
branch/texture, voice ownership, scene worker, seek cancellation, transport,
terminal protocol, live subprocess scrubbing, deterministic export and all-demo tests.
`audio-pipe` runs the POSIX backend even on macOS so that platform code does not
silently escape CI. Each failing baseline test is described above.

Validation on macOS: `make test` passes all 42 tracked demos and the new
regressions. `make test-sanitize` passes AddressSanitizer, UndefinedBehaviorSanitizer
and float-cast-overflow checks for loop safety, numeric lookup, compile retry,
reload ownership and asynchronous seeking. Separate ThreadSanitizer builds of
`tests/audio-pipe.c` and `tests/seek.c` pass. Both macOS and Linux also passed
the engine/demo suite and sanitizer steps in [CI for `511dbf4`](https://github.com/HelgeSverre/dmscene/actions/runs/42303161332).
The sanitizer target remains wired into that CI matrix. Existing live subprocess tests cover scroll, reverse
seek, pause and slow-frame-rate input wakeup. They also run with a real pipe
backend whose player immediately exits, checking continued wall-clock playback.
The first remote macOS run exposed a fixed-400-ms assumption in the existing
speed-change test. The test now waits up to three seconds for **both** the
reported running speed and new clock progress; it also detects an ignored speed
key, which the old comparison against a much earlier timestamp could miss.
The independent 250-ms maximum scroll-frame gap remains enforced.
No claim is made that numeric fuzzing, allocation-failure injection at every
allocation, hardware audio interruption, or every external encoder behavior
has been exhaustively tested.
