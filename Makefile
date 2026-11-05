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
PLAT   := src/dm_term.c src/dm_audio.c
OBJ    := $(ENGINE:src/%.c=build/%.o)
POBJ   := $(PLAT:src/%.c=build/%.o)

DEMO   := demos/pulse.dmt
DEMOS  := $(wildcard demos/*.dmt)
DMS    := $(DEMOS:demos/%.dmt=build/%.dms)
MP4    := $(DEMOS:demos/%.dmt=build/video/%.mp4)
SECONDS ?= 30
SIZE    ?= 960x600

all: dm dmc dmplay

build/%.o: src/%.c include/dm.h include/dm_ops.h include/dm_banner.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

dm: tools/dm.c $(OBJ) $(POBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

dmc dmplay: dm
	ln -sf dm $@

build/%.dms: demos/%.dmt dm
	./dm compile $< -o $@

build/video/%.mp4: build/%.dms dm
	@mkdir -p build/video
	./dm render $< --mp4 $@ --seconds $(SECONDS) --size $(SIZE)

demo: build/pulse.dms
demos: $(DMS)
videos: $(MP4)

run: dm
	./dm play demos/pulse.dmt

test: all demos
	./tests/run.sh

clean:
	rm -rf build dm dmc dmplay

.PHONY: all demo demos videos run test clean
