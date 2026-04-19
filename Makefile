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

FLOPPY_SRC ?= $(HOME)/fun/msdos/out/floppy.img
FLOPPY_OUT  = build/adlib.img

.PHONY: all clean run floppy check-floppy-src

all: $(EXE)

build:
	@mkdir -p build

build/%.obj: src/%.c | build
	$(WCC) $(WCCFLAGS) -fo=$@ $<

$(EXE): $(OBJ)
	$(WLINK) name $@ format dos $(addprefix file ,$(OBJ)) libpath $(WATCOM_LIB) library clibs.lib

check-floppy-src:
	@if [ ! -f "$(FLOPPY_SRC)" ]; then \
	  echo ""; \
	  echo "ERROR: MS-DOS floppy not found at $(FLOPPY_SRC)"; \
	  echo ""; \
	  echo "Build it first in the ddanila/msdos checkout, e.g.:"; \
	  echo "    cd ~/fun/msdos && make minimal-floppy"; \
	  echo ""; \
	  echo "Or override the source path:"; \
	  echo "    make floppy FLOPPY_SRC=/path/to/dos4.img"; \
	  echo ""; \
	  exit 1; \
	fi

floppy: $(FLOPPY_OUT)

$(FLOPPY_OUT): $(EXE) check-floppy-src
	cp "$(FLOPPY_SRC)" $@
	mcopy -i $@ -o $(EXE) ::ADLIB.EXE
	@printf '@ECHO OFF\r\nADLIB\r\n' > build/AUTOEXEC.BAT
	mcopy -i $@ -o build/AUTOEXEC.BAT ::AUTOEXEC.BAT
	@echo "Floppy ready: $@"

run: $(FLOPPY_OUT)
	bash scripts/run.sh $(FLOPPY_OUT)

clean:
	rm -rf build
