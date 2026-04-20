# adlib-rng — build a 16-bit DOS .EXE with OpenWatcom v2 and pack it
# onto an MS-DOS 4.0 boot floppy for QEMU.

WATCOM_DIR ?= vendor/openwatcom-v2/current-build-2026-04-20
HOST_OS    := $(shell uname -s)
HOST_ARCH  := $(shell uname -m)
ifeq ($(HOST_OS),Darwin)
  ifeq ($(HOST_ARCH),arm64)
    WATCOM_BIN := $(WATCOM_DIR)/macos-arm64
  else
    WATCOM_BIN := $(WATCOM_DIR)/macos-x64
  endif
else
  WATCOM_BIN := $(WATCOM_DIR)/linux-amd64
endif

WCC        = $(WATCOM_BIN)/wcc
WLINK      = $(WATCOM_BIN)/wlink
WATCOM_H   = $(WATCOM_DIR)/h
WATCOM_LIB = $(WATCOM_DIR)/lib286/dos

# -0    = 8086 instruction set (universal)
# -ms   = small memory model (64K code + 64K data, plenty here)
# -os   = optimize for size
# -s    = no stack overflow checks
# -za99 = C99 mode (mixed declarations, // comments)
# -w4 -we = max warnings, treat as errors
# -oi   = inline intrinsics (memset/memcpy)
WCCFLAGS = -0 -ms -os -s -za99 -w4 -we -oi -i=$(WATCOM_H)

SRC     = src/main.c src/opl2.c src/timer.c src/rng.c src/music.c \
          src/display.c src/seeds.c src/player_rng.c src/player_vgm.c
OBJ     = $(SRC:src/%.c=build/%.obj)
HEADERS = $(wildcard src/*.h)
EXE     = build/adlib.exe

# Bootable MS-DOS 4.0 floppy — vendored under vendor/msdos/ so
# `make floppy` never hits the network. Override FLOPPY_SRC to use a
# different image. See vendor/msdos/README.md for the source and
# refresh instructions.
FLOPPY_SRC      ?= vendor/msdos/floppy-minimal.img
FLOPPY_OUT       = build/adlib.img

.PHONY: all clean run floppy

all: $(EXE)

build:
	@mkdir -p build

# Conservative: rebuild every TU when any header changes. With 6 TUs
# and tiny compile times this is fine, and it avoids the bar_t-size
# desync we hit when chord_root_midi grew to an array.
build/%.obj: src/%.c $(HEADERS) | build
	$(WCC) $(WCCFLAGS) -fo=$@ $<

$(EXE): $(OBJ)
	$(WLINK) name $@ format dos $(addprefix file ,$(OBJ)) libpath $(WATCOM_LIB) library clibs.lib

floppy: $(FLOPPY_OUT)

# Everything under assets/ gets shipped on the floppy with an 8.3
# uppercase filename. OPL3 originals live under sources/ and are
# folded to OPL2 via scripts/transcode_vgm.py. Run `make vgms` to
# regenerate the derived files from the sources.
VGM_SRCS     = $(wildcard assets/*.vgm)
VGM_SOURCES  = $(wildcard sources/*.vgm)
VGM_TRANSCODED = $(patsubst sources/%.vgm,assets/%.vgm,$(VGM_SOURCES))

assets/%.vgm: sources/%.vgm scripts/transcode_vgm.py
	python3 scripts/transcode_vgm.py $< $@

assets/testopl.vgm: scripts/make_test_vgm.py
	python3 scripts/make_test_vgm.py

.PHONY: vgms
vgms: $(VGM_TRANSCODED) assets/testopl.vgm

$(FLOPPY_OUT): $(EXE) $(FLOPPY_SRC) $(VGM_SRCS)
	cp "$(FLOPPY_SRC)" $@
	mcopy -i $@ -o $(EXE) ::ADLIB.EXE
	@for f in $(VGM_SRCS); do \
	  base=$$(basename "$$f" .vgm | tr 'a-z' 'A-Z' | cut -c1-8); \
	  echo "  -> $$base.VGM"; \
	  mcopy -i $@ -o "$$f" "::$$base.VGM"; \
	done
	@printf '@ECHO OFF\r\nADLIB SUSPENSE.VGM\r\n' > build/AUTOEXEC.BAT
	mcopy -i $@ -o build/AUTOEXEC.BAT ::AUTOEXEC.BAT
	@echo "Floppy ready: $@"

run: $(FLOPPY_OUT)
	bash scripts/run.sh $(FLOPPY_OUT)

clean:
	rm -rf build
