#!/bin/bash
# run.sh — boot the adlib-rng floppy in QEMU with OPL2 (AdLib) audio.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLOPPY="${1:-$SCRIPT_DIR/../build/adlib.img}"

if [[ ! -f "$FLOPPY" ]]; then
    echo "ERROR: floppy image not found: $FLOPPY"
    echo "Run 'make floppy' first."
    exit 1
fi

case "$(uname -s)" in
    Darwin) AUDIODRV=coreaudio ;;
    Linux)  AUDIODRV=pa ;;
    *)      AUDIODRV=sdl ;;
esac

echo "Booting: $FLOPPY  (audio=$AUDIODRV)"
exec qemu-system-i386 \
    -drive if=floppy,format=raw,file="$FLOPPY" \
    -boot a \
    -m 4 \
    -audiodev "$AUDIODRV,id=snd0" \
    -machine pcspk-audiodev=snd0 \
    -device adlib,audiodev=snd0 \
    -display sdl \
    -name "adlib-rng"
