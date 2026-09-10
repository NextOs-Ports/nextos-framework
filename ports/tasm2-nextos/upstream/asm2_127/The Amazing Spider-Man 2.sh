#!/bin/sh

PORT_LAUNCHER=/storage/roms/ports/ASM2-1.2.7d.sh

if [ ! -x "$PORT_LAUNCHER" ]; then
    printf 'ASM2 launcher not found: %s\n' "$PORT_LAUNCHER" >&2
    exit 1
fi

exec "$PORT_LAUNCHER"
