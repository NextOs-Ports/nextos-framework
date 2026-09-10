#!/bin/bash
# Build do so-loader do Retro City Rampage DX — AArch64, toolchain/sysroot do
# NextOS Elite atual (fase `rad`: só Mali-450).  O gate de glibc baixa e o
# pacote público pertencem à fase `radu` e não entram aqui.
set -e
NEXTOS_ROOT="${NEXTOS_ROOT:-$HOME/NextOS-Elite-Edition}"
# caminho FÍSICO, nunca o symlink de ~ (regra de build do NextOS)
TC=$(
  find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print 2>/dev/null | sort -V | tail -1
)
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
READELF=$TC/bin/aarch64-libreelec-linux-gnu-readelf
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain aarch64 não encontrada: $CC"; exit 1; }
[ -d "$SR/usr/include/SDL2" ] || { echo "sysroot sem headers SDL2: $SR"; exit 1; }

# /tmp com usrquota quebra o build (lição registrada) — usar TMPDIR próprio
export TMPDIR="${TMPDIR:-$PWD/tmpbuild}"
mkdir -p "$TMPDIR"

echo "toolchain: $TC"

SRCS="src/main.c src/nx_elf.c src/bionic.c src/pthread_bridge.c src/jni.c \
      src/sdl_java.c src/android.c src/egl.c src/video.c src/input.c \
      src/audio.c src/probe_ring.c src/nx_frameprobe.c"

"$CC" --sysroot="$SR" -fPIE -pie -O2 -g -fno-omit-frame-pointer -funwind-tables \
  -rdynamic -D_GNU_SOURCE \
  -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
  -Wno-comment -Wno-int-conversion -Wno-incompatible-pointer-types \
  -o rcrdx $SRCS \
  -Isrc -I"$SR/usr/include" -I"$SR/usr/include/SDL2" \
  -Wl,--export-dynamic \
  -lSDL2 -lEGL -lGLESv2 -lz -ldl -lm -lpthread

echo "BUILD OK -> $(file rcrdx | cut -d, -f1-3)"
echo "GLIBC requerida -> $("$READELF" -V rcrdx 2>/dev/null |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sed 's/^GLIBC_//' | sort -Vu | tail -1)"
echo "tamanho: $(stat -c%s rcrdx) bytes"
"$READELF" -d rcrdx | grep NEEDED
