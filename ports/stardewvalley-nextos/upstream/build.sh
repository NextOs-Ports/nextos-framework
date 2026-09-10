#!/usr/bin/env bash
# Build the native NextOS/NextOS Elite loader against the current project
# sysroot. External CFW compatibility is a separate, explicit build_buster.sh
# artifact and never replaces this release binary.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

TC=${SDV_NEXTOS_TOOLCHAIN:-"$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain"}
CC="$TC/bin/aarch64-libreelec-linux-gnu-gcc"
SR="$TC/aarch64-libreelec-linux-gnu/sysroot"
LIBC="$SR/usr/lib/libc.so.6"
[ -x "$CC" ] || { printf 'missing NextOS compiler: %s\n' "$CC" >&2; exit 1; }
[ -f "$LIBC" ] || { printf 'missing NextOS sysroot libc: %s\n' "$LIBC" >&2; exit 1; }
GLIBC_RELEASE=$(strings "$LIBC" |
  sed -n 's/^GNU C Library .* stable release version \([0-9.]*\)[.]$/\1/p')
[ -n "$GLIBC_RELEASE" ] || { printf 'cannot identify sysroot glibc\n' >&2; exit 1; }
printf 'NextOS sysroot glibc: %s\n' "$GLIBC_RELEASE"

# Loader Mono + ponte SDL/EGL minima (SDL resolvido via dlsym no device).
SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.gen.c \
      src/bionic_shims.c src/pthread_bridge.c src/sdv_egl_bridge.c \
      src/util.c src/error.c"

"$CC" --sysroot="$SR" -I src -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -DPORT_WINDOW_TITLE='"Stardew Valley"' \
    -Wno-unused-parameter -Wno-unused-function \
    -o stardewvalley $SRCS \
    -ldl -lm -lpthread

echo "OK: $HERE/stardewvalley"
"$TC/bin/aarch64-libreelec-linux-gnu-objdump" -p stardewvalley 2>/dev/null | \
  grep NEEDED || true
mkdir -p .build-provenance
python3 tools/build_provenance.py record \
  --root "$HERE" \
  --profile nextos-current \
  --binary "$HERE/stardewvalley" \
  --compiler "$("$CC" --version | head -1)" \
  --sysroot-libc "$LIBC" \
  --output "$HERE/.build-provenance/nextos-current.json"
