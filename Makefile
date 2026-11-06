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

ENGINE := src/dm_vm.c src/dm_font.c src/dm_seq.c src/dm_synth.c src/dm_module.c src/dm_compile.c src/dm_scene.c src/dm_io.c
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

build/tests/showcase-gates: tests/showcase-gates.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/synth-voices: tests/synth-voices.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/branch-texture: tests/branch-texture.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/scene-workers: tests/scene-workers.c $(OBJ) | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/term-input: tests/term-input.c src/dm_term.c include/dm.h | build/tests
	$(CC) $(CFLAGS) tests/term-input.c src/dm_term.c -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/play-transport: tests/play-transport.c tools/dm.c $(OBJ) $(POBJ) | build/tests
	$(CC) $(CFLAGS) tests/play-transport.c $(OBJ) $(POBJ) -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/seek: tests/seek.c $(OBJ) build/obj/dm_seek.o build/obj/dm_term.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/scrub-live: tests/scrub-live.c build/obj/dm_term.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/dm-audio-clock: tools/dm.c tests/audio-clock.c $(OBJ) build/obj/dm_term.o build/obj/dm_seek.o | build/tests
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: all demos build/tests/showcase-gates build/tests/synth-voices build/tests/branch-texture build/tests/scene-workers build/tests/term-input build/tests/play-transport build/tests/seek build/tests/scrub-live build/tests/dm-audio-clock
	./tests/run.sh

clean:
	rm -rf build dm

.PHONY: all demo demos videos showcase showcase-preview run test clean
