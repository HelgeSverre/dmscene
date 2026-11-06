# dmscene - pure C99, no third-party libraries.
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Iinclude -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS ?=
LDLIBS  ?= -lm

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDLIBS += -framework AudioToolbox -framework CoreFoundation
else
  LDLIBS += -lpthread
endif

ENGINE := src/dm_vm.c src/dm_font.c src/dm_seq.c src/dm_synth.c src/dm_module.c src/dm_compile.c src/dm_scene.c src/dm_io.c src/dm_mov.c
PLAT   := src/dm_term.c src/dm_audio.c src/dm_seek.c
OBJ    := $(ENGINE:src/%.c=build/obj/%.o)
POBJ   := $(PLAT:src/%.c=build/obj/%.o)

DEMO   ?= demos/showcase.dmt
DEMOS  := $(wildcard demos/*.dmt)
DMS    := $(DEMOS:demos/%.dmt=build/modules/%.dms)
MP4    := $(DEMOS:demos/%.dmt=build/video/%.mp4)
SECONDS ?= 30
SIZE    ?= 960x600
SHOWCASE_SECONDS := 42.631836735
SHOWCASE_CHAPTER := 5.328979592

all: dm

# Optional per-demo shader AOT runner; normal AVI/MOV export is built into dm.
avi-prototype: tools/avi-prototype.c include/dm_mov.h $(OBJ) $(POBJ)
	$(CC) $(CFLAGS) $(filter %.c %.o,$^) -o $@ $(LDFLAGS) $(LDLIBS)

build/dm-aot: tools/dm-aot.c $(OBJ) $(POBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/native/%.c: build/modules/%.dms build/dm-aot src/dm_vm.c
	mkdir -p build/native
	./build/dm-aot $< > $@.tmp
	mv $@.tmp $@

build/native/%: build/native/%.c tools/avi-prototype.c include/dm_mov.h $(filter-out build/obj/dm_vm.o,$(OBJ)) $(POBJ)
	$(CC) $(CFLAGS) -I. -DDM_AOT -Wno-unused-variable -Wno-unused-label $(filter %.c %.o,$^) -o $@ $(LDFLAGS) $(LDLIBS)

native-export: build/native/$(notdir $(DEMO:.dmt=))
	@echo "Run $< $(DEMO) output.avi [--seconds S] [--size WxH] [--threads N]"

# Compile this demo's shaders into a normal dm CLI; audio remains interpreted.
build/native-player/%: build/native/%.c tools/dm.c $(filter-out build/obj/dm_vm.o,$(OBJ)) $(POBJ)
	mkdir -p build/native-player
	$(CC) $(CFLAGS) -I. -DDM_AOT -Wno-unused-variable -Wno-unused-label $(filter %.c %.o,$^) -o $@ $(LDFLAGS) $(LDLIBS)

native-player: build/native-player/$(notdir $(DEMO:.dmt=))
	@echo "Run $< play $(DEMO) [play options]"

play-native: build/native-player/$(notdir $(DEMO:.dmt=))
	$< play $(DEMO) $(PLAY_ARGS)

test-native: avi-prototype build/dm-aot dm build/tests/scrub-live
	sh tests/native-export.sh
	sh tests/native-player.sh
	@if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then sh tests/qtrle-equivalence.sh; fi

.PRECIOUS: build/native/%.c

build/obj/dm_mov.o build/sanitize/dm_mov.o: include/dm_mov.h


build/obj/%.o: src/%.c include/dm.h include/dm_ops.h include/dm_banner.h include/dm_seek.h | build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/obj build/modules build/audio build/video build/tests:
	mkdir -p $@

dm: tools/dm.c $(OBJ) $(POBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/modules/%.dms: demos/%.dmt dm | build/modules
	./dm compile $< -o $@

build/video/%.mp4: build/modules/%.dms dm | build/video build/audio
	./dm render $< --mp4 $@ --wav build/audio/$*.wav --seconds $(SECONDS) --size $(SIZE)

demo: build/modules/$(notdir $(DEMO:.dmt=.dms))
demos: $(DMS)
videos: $(MP4)

# 32 bars at 180 BPM. The tracker floors each tick to 612 samples at 44.1 kHz.
showcase: build/modules/showcase.dms | build/video build/audio
	./dm render $< --mp4 build/video/showcase.mp4 --wav build/audio/showcase.wav --seconds $(SHOWCASE_SECONDS) --size $(SIZE) --fps 30

# Silent chapter highlights for the README; the MP4 contains the themesong.
showcase-preview: showcase
	ffmpeg -y -hide_banner -loglevel error -i build/video/showcase.mp4 -filter_complex "[0:v]select='lt(mod(t,$(SHOWCASE_CHAPTER)),1.35)',setpts=N/30/TB,fps=8,scale=640:400:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" -loop 0 art/showcase.gif

run: dm
	./dm play $(DEMO)

# Static showcase gallery. Uses build/video/*.mp4 and demos/*.dmt as inputs.
site:
	node web/_sitegen/sitegen.mjs

# Browser engine; separate artifacts and an explicit single-worker build.
wasm:
	node web/_runtime/build.mjs

web-runtime: wasm

web: wasm
	$(MAKE) site

test-web: dm
	node --test web/_sitegen/*.test.mjs

test-wasm: wasm
	node --test web/_runtime/*.test.mjs

clean-wasm:
	rm -rf build/wasm web/runtime

serve:
	node web/_sitegen/serve.mjs

build/tests/showcase-gates: tests/showcase-gates.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/synth-voices: tests/synth-voices.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/branch-texture: tests/branch-texture.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/scene-workers: tests/scene-workers.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/term-input build/tests/term-graphics: build/tests/%: tests/%.c src/dm_term.c include/dm.h | build/tests
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/native-bind: tests/native-bind.c build/native/hakkuh.c $(filter-out build/obj/dm_vm.o,$(OBJ)) $(POBJ) | build/tests
	$(CC) $(CFLAGS) -I. -Wno-unused-variable -Wno-unused-label $(filter %.c %.o,$^) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/play-transport: tests/play-transport.c tools/dm.c $(OBJ) $(POBJ) | build/tests
	$(CC) $(CFLAGS) tests/play-transport.c $(OBJ) $(POBJ) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/seek: tests/seek.c $(OBJ) build/obj/dm_seek.o build/obj/dm_term.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/scrub-live: tests/scrub-live.c build/obj/dm_term.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/dm-audio-clock: tools/dm.c tests/audio-clock.c $(OBJ) build/obj/dm_term.o build/obj/dm_seek.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/state-safety build/tests/output-errors build/tests/vm-numeric build/tests/native-writer-errors: build/tests/%: tests/%.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/compile-retry: tests/compile-retry.c src/dm_compile.c src/dm_module.c $(filter-out build/obj/dm_compile.o build/obj/dm_module.o,$(OBJ)) | build/tests
	$(CC) $(CFLAGS) tests/compile-retry.c $(filter-out build/obj/dm_compile.o build/obj/dm_module.o,$(OBJ)) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/watch-state: tests/watch-state.c tools/dm.c $(OBJ) $(POBJ) | build/tests
	$(CC) $(CFLAGS) tests/watch-state.c $(OBJ) $(POBJ) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/audio-pipe: tests/audio-pipe.c src/dm_audio.c src/dm_term.c include/dm.h | build/tests
	$(CC) $(CFLAGS) -DDM_AUDIO_PIPE tests/audio-pipe.c src/dm_audio.c src/dm_term.c -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/dm-pipe: tools/dm.c src/dm_audio.c $(OBJ) build/obj/dm_term.o build/obj/dm_seek.o | build/tests
	$(CC) $(CFLAGS) -DDM_AUDIO_PIPE $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: all demos build/tests/showcase-gates build/tests/synth-voices build/tests/branch-texture build/tests/scene-workers build/tests/term-input build/tests/term-graphics build/tests/play-transport build/tests/seek build/tests/scrub-live build/tests/dm-audio-clock build/tests/state-safety build/tests/output-errors build/tests/compile-retry build/tests/watch-state build/tests/audio-pipe build/tests/vm-numeric build/tests/dm-pipe build/tests/native-writer-errors
	./tests/run.sh

# Separate objects keep sanitizer flags out of the normal dm binary.
SANFLAGS := $(CFLAGS) -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all
SANOBJ := $(ENGINE:src/%.c=build/sanitize/%.o)
SANPOBJ := $(PLAT:src/%.c=build/sanitize/%.o)
SAN_TESTS := state-safety vm-numeric compile-retry watch-state seek native-writer-errors term-graphics scene-workers

build/sanitize build/sanitize/tests:
	mkdir -p $@

build/sanitize/%.o: src/%.c include/dm.h include/dm_ops.h include/dm_seek.h | build/sanitize
	$(CC) $(SANFLAGS) -c $< -o $@

build/sanitize/tests/state-safety build/sanitize/tests/vm-numeric build/sanitize/tests/seek build/sanitize/tests/native-writer-errors build/sanitize/tests/scene-workers: build/sanitize/tests/%: tests/%.c $(SANOBJ) $(SANPOBJ) | build/sanitize/tests
	$(CC) $(SANFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/sanitize/tests/term-graphics: tests/term-graphics.c build/sanitize/dm_term.o | build/sanitize/tests
	$(CC) $(SANFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/sanitize/tests/watch-state: tests/watch-state.c tools/dm.c $(SANOBJ) $(SANPOBJ) | build/sanitize/tests
	$(CC) $(SANFLAGS) tests/watch-state.c $(SANOBJ) $(SANPOBJ) -o $@ $(LDFLAGS) $(LDLIBS)

build/sanitize/tests/compile-retry: tests/compile-retry.c src/dm_compile.c src/dm_module.c $(filter-out build/sanitize/dm_compile.o build/sanitize/dm_module.o,$(SANOBJ)) | build/sanitize/tests
	$(CC) $(SANFLAGS) tests/compile-retry.c $(filter-out build/sanitize/dm_compile.o build/sanitize/dm_module.o,$(SANOBJ)) -o $@ $(LDFLAGS) $(LDLIBS)

test-sanitize: $(addprefix build/sanitize/tests/,$(SAN_TESTS))
	@set -e; for t in $(SAN_TESTS); do ./build/sanitize/tests/$$t; done

clean:
	rm -rf build dm

.PHONY: all demo demos videos showcase showcase-preview run site wasm web web-runtime serve test-web test-wasm clean-wasm test test-sanitize clean avi-prototype native-export native-player play-native test-native
