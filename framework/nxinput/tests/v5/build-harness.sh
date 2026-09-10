#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V5: reproducible build of the pure SDL2 harness with the REAL C6 glue.
# Usage: tests/v5/build-harness.sh <out-elf> [CC] [extra cflags...]
# Cross: NX_SDL_CFLAGS='-I/usr/include/SDL2' NX_SDL_LIBS='-L<dir> -l:libSDL2-2.0.so.0.X' CC=aarch64-linux-gnu-gcc
# (the link-time DSO only provides symbols; at run time the provider is whatever the loader maps)
# The harness links the system SDL2 by SONAME (never a private copy); the
# provider is resolved at run time from the object the process mapped.
set -eu
HERE=$(cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?out elf}; CC=${2:-gcc}; shift 2 2>/dev/null || shift $#
SRC="tests/v5/harness_sdl2_provider.c engine-glue/nxc6_glue.c src/nxinput_sdl_seam.c
src/nxinput_sdl.c src/nxinput_portmaster.c src/nxinput_sovereign.c src/nxinput_authority.c
src/nxinput_livedb.c src/nxinput_godot.c src/nxinput_provider.c src/nxinput_provider_linux.c
src/nxinput_sha256.c src/nxinput_translate.c src/nxinput_decision.c"
cd "$HERE"
# shellcheck disable=SC2086
$CC -std=c99 -Wall -Wextra -Wno-comment -Iinclude -Iengine-glue \
  ${NX_SDL_CFLAGS:-$(pkg-config --cflags sdl2 2>/dev/null || echo -I/usr/include/SDL2)} "$@" \
  -o "$OUT" $SRC ${NX_SDL_LIBS:-$(pkg-config --libs sdl2 2>/dev/null || echo -lSDL2)} -ldl -lm -lpthread
echo "harness: $OUT sha256=$(sha256sum "$OUT" | cut -c1-16)"
