# adlib-rng — build a 16-bit DOS .EXE with OpenWatcom v2 and pack it
# onto an MS-DOS 4.0 boot floppy for QEMU.

WATCOM_DIR ?= $(HOME)/fun/beta_kappa/vendor/openwatcom-v2/current-build-2026-04-04
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

SRC = src/main.c src/opl2.c src/timer.c src/rng.c src/music.c src/display.c
OBJ = $(SRC:src/%.c=build/%.obj)
EXE = build/adlib.exe

# Bootable MS-DOS 4.0 floppy. Pulled as a release asset from the
# ddanila/msdos project. Override FLOPPY_SRC to use your own image.
MSDOS_RELEASE   ?= 0.1
MSDOS_FLOPPY    ?= floppy-minimal.img
FLOPPY_SRC      ?= build/dos/$(MSDOS_FLOPPY)
FLOPPY_OUT       = build/adlib.img

.PHONY: all clean run floppy fetch-floppy

all: $(EXE)

build:
	@mkdir -p build build/dos

build/%.obj: src/%.c | build
	$(WCC) $(WCCFLAGS) -fo=$@ $<

$(EXE): $(OBJ)
	$(WLINK) name $@ format dos $(addprefix file ,$(OBJ)) libpath $(WATCOM_LIB) library clibs.lib

fetch-floppy: $(FLOPPY_SRC)

# Cached download from the ddanila/msdos release. Re-run with
# `rm -rf build/dos` if you want to refetch.
build/dos/$(MSDOS_FLOPPY): | build
	@if ! command -v gh >/dev/null 2>&1; then \
	  echo "ERROR: gh CLI not found. Install it (brew install gh) or set FLOPPY_SRC=/path/to/img."; \
	  exit 1; \
	fi
	@echo "Fetching $(MSDOS_FLOPPY) from ddanila/msdos@$(MSDOS_RELEASE)..."
	@mkdir -p build/dos
	gh release download $(MSDOS_RELEASE) --repo ddanila/msdos --pattern $(MSDOS_FLOPPY) --dir build/dos --clobber

floppy: $(FLOPPY_OUT)

$(FLOPPY_OUT): $(EXE) $(FLOPPY_SRC)
	cp "$(FLOPPY_SRC)" $@
	mcopy -i $@ -o $(EXE) ::ADLIB.EXE
	@printf '@ECHO OFF\r\nADLIB\r\n' > build/AUTOEXEC.BAT
	mcopy -i $@ -o build/AUTOEXEC.BAT ::AUTOEXEC.BAT
	@echo "Floppy ready: $@"

run: $(FLOPPY_OUT)
	bash scripts/run.sh $(FLOPPY_OUT)

clean:
	rm -rf build
