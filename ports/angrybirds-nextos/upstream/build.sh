#!/usr/bin/env bash
# Deterministic public ARMv7/ARMHF build for Angry Birds Classic 8.0.3.
# The pinned Debian Buster sysroot supplies glibc 2.28. Firmware-owned
# SDL2/EGL/GLESv2/zlib are represented only by SONAME link stubs.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
REPO_DIR=$PORT_DIR
VENDOR_DIR=$PORT_DIR/vendor
OUTPUT=${AB_OUTPUT:-$PORT_DIR/angrybirds-nextos}
PREBUILT=/opt/prebuilt
CC=$PREBUILT/bin/arm-linux-gnueabihf-gcc
NM=$PREBUILT/bin/arm-linux-gnueabihf-nm
READELF=$PREBUILT/bin/arm-linux-gnueabihf-readelf
STRIP=$PREBUILT/bin/arm-linux-gnueabihf-strip
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786233600}

fail() {
  printf 'angrybirds build error: %s\n' "$*" >&2
  exit 1
}

[[ -x $CC && -x $NM && -x $READELF && -x $STRIP ]] ||
  fail "pinned ARMHF toolchain is missing below $PREBUILT"

# NEXTOS_ROOT must point at a NextOS Elite build tree; it only provides the
# read-only SDL2/EGL/GLES2/zlib headers. NEXTOS_SYSROOT overrides it directly.
NEXTOS_ROOT=${NEXTOS_ROOT:-}
NEXTOS_SYSROOT=${NEXTOS_SYSROOT:-}
if [[ -z $NEXTOS_SYSROOT ]]; then
  [[ -n $NEXTOS_ROOT ]] ||
    fail "set NEXTOS_SYSROOT, or NEXTOS_ROOT to a NextOS Elite build tree"
  NEXTOS_TOOLCHAIN=$(find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print | sort -V | tail -1)
  [[ -n $NEXTOS_TOOLCHAIN ]] || fail "NextOS header sysroot not found"
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/armv8a-emuelec-linux-gnueabihf/sysroot
fi
[[ -f $NEXTOS_SYSROOT/usr/include/SDL2/SDL.h &&
   -f $NEXTOS_SYSROOT/usr/include/EGL/egl.h &&
   -f $NEXTOS_SYSROOT/usr/include/GLES2/gl2.h &&
   -f $NEXTOS_SYSROOT/usr/include/zlib.h ]] ||
  fail "SDL2/EGL/GLES2/zlib headers are incomplete in NEXTOS_SYSROOT"

# NXABI is the internal NextOS toolchain gate; it is not distributed with this
# repository. Point NXABI at a checkout to re-run the pinned-toolchain audit.
if [[ -n ${NXABI:-} ]]; then
  [[ -f $NXABI ]] || fail "NXABI does not point at nxabi.py: $NXABI"
  python3 -B "$NXABI" toolchain >/dev/null ||
    fail "nxabi rejected the pinned toolchain"
else
  printf 'angrybirds build: NXABI not set, skipping the toolchain audit\n' >&2
fi

if [[ -n ${AB_LUAC_ARM:-} ]]; then
  "$PORT_DIR/tools/build-lua-adapter.sh"
fi

OBJDIR=$(mktemp -d /tmp/angrybirds-obj.XXXXXX)
STUBDIR=$(mktemp -d /tmp/angrybirds-stub.XXXXXX)
cleanup() {
  find "$OBJDIR" "$STUBDIR" -depth -mindepth 1 -delete 2>/dev/null || true
  rmdir "$OBJDIR" "$STUBDIR" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

SOURCES=(
  "$PORT_DIR/src/main_angrybirds.c"
  "$PORT_DIR/src/ab_log.c"
  "$PORT_DIR/src/ab_apk.c"
  "$PORT_DIR/src/ab_bionic.c"
  "$PORT_DIR/src/ab_pthread.c"
  "$PORT_DIR/src/ab_stdio.c"
  "$PORT_DIR/src/ab_gl.c"
  "$PORT_DIR/src/ab_egl.c"
  "$PORT_DIR/src/ab_jni.c"
  "$PORT_DIR/src/ab_locale.c"
  "$PORT_DIR/src/ab_audio.c"
  "$PORT_DIR/src/ab_evdev_exit.c"
  "$PORT_DIR/src/ab_input.c"
  "$PORT_DIR/src/ab_lua_control.c"
  "$PORT_DIR/src/ab_framework.c"
  "$VENDOR_DIR/nxloader/src/nxloader.c"
  "$VENDOR_DIR/nxloader/src/nxloader_elf32.c"
  "$VENDOR_DIR/nxloader/src/nxloader_elf64.c"
  "$VENDOR_DIR/nxloader/src/nxloader_hooks.c"
  "$VENDOR_DIR/nxloader/src/nxloader_protect.c"
  "$VENDOR_DIR/nxloader/src/nxloader_registry.c"
  "$VENDOR_DIR/nxloader/src/nxloader_softfp.c"
  # nxcompat 0.4.0 e nxgl 0.3.5 vendorizados da V4 congelada (framework-v4):
  # todos os .c do vendor entram, em ordem determinística.
  $(find "$VENDOR_DIR/nxcompat/src" "$VENDOR_DIR/nxcompat/adapters/sdl2" "$VENDOR_DIR/nxgl/src" -name '*.c' -not -name 'nxgl_gles1.c' | LC_ALL=C sort)
  "$VENDOR_DIR/nxinput/src/nxinput.c"
  "$VENDOR_DIR/nxinput/src/nxinput_core.c"
  "$VENDOR_DIR/nxinput/src/nxinput_nxcompat.c"
  "$VENDOR_DIR/nxaudio/src/nxaudio.c"
)

INCLUDES=(
  -I"$PORT_DIR/src"
  -I"$VENDOR_DIR/nxloader/include"
  -I"$VENDOR_DIR/nxcompat/include"
  -I"$VENDOR_DIR/nxcompat/src"
  -I"$VENDOR_DIR/nxcompat/adapters/sdl2"
  -I"$VENDOR_DIR/nxgl/include"
  -I"$VENDOR_DIR/nxgl/src"
  -I"$VENDOR_DIR/nxinput/include"
  -I"$VENDOR_DIR/nxinput/src"
  -I"$VENDOR_DIR/nxaudio/include"
  -idirafter "$NEXTOS_SYSROOT/usr/include"
  -idirafter "$NEXTOS_SYSROOT/usr/include/SDL2"
)
CFLAGS=(
  --sysroot="$PREBUILT/arm-linux-gnueabihf/libc"
  -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard
  -include "$PORT_DIR/src/ab_build_features.h"
  -include "$VENDOR_DIR/nxabi/include/nx_symver.h"
  -include "$PORT_DIR/src/ab_symver.h"
  -fno-builtin-powf -fno-builtin-expf -fno-builtin-exp2f
  -fno-builtin-logf -fno-builtin-log2f
  -O2 -fPIE -fno-strict-aliasing -fno-omit-frame-pointer
  -ffile-prefix-map="$REPO_DIR"=. -fdebug-prefix-map="$REPO_DIR"=.
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
  -Wno-format-truncation
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
)

OBJECTS=()
index=0
for source in "${SOURCES[@]}"; do
  object=$OBJDIR/$(printf '%03d-%s.o' "$index" "$(basename "${source%.c}")")
  "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$source" -o "$object"
  OBJECTS+=("$object")
  index=$((index + 1))
done

UNDEFINED=$($NM --undefined-only "${OBJECTS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)
generate_stub() {
  local pattern=$1 output=$2 symbol
  : > "$output"
  while IFS= read -r symbol; do
    [[ $symbol =~ $pattern ]] || continue
    printf 'void %s(void) {}\n' "$symbol" >> "$output"
  done <<< "$UNDEFINED"
}
generate_stub '^SDL_' "$STUBDIR/sdl.c"
generate_stub '^(adler32|crc32|inflate|inflateEnd|inflateInit2_|zlibVersion)$' \
  "$STUBDIR/z.c"

"$CC" "${CFLAGS[@]}" -shared -nostdlib -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUBDIR/sdl.c" -o "$STUBDIR/libSDL2.so"
"$CC" "${CFLAGS[@]}" -shared -nostdlib -Wl,-soname,libz.so.1 \
  "$STUBDIR/z.c" -o "$STUBDIR/libz.so"

"$CC" "${CFLAGS[@]}" -pie -rdynamic -o "$OUTPUT" "${OBJECTS[@]}" \
  -L"$STUBDIR" -Wl,--no-as-needed -lSDL2 -lz \
  -Wl,--as-needed -ldl -lm -lpthread -latomic -lgcc_s \
  -Wl,--unresolved-symbols=ignore-all \
  -Wl,--build-id=sha1 -Wl,-z,relro,-z,lazy,-z,noexecstack
"$STRIP" --strip-debug "$OUTPUT"
chmod 0755 "$OUTPUT"

MACHINE=$("$READELF" -h "$OUTPUT" |
  sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[[ $MACHINE == ARM ]] || fail "unexpected ELF machine: $MACHINE"
INTERPRETER=$("$READELF" -lW "$OUTPUT" |
  sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ $INTERPRETER == /lib/ld-linux-armhf.so.3 ]] ||
  fail "unexpected PT_INTERP: $INTERPRETER"
MAX_GLIBC=$("$READELF" --version-info "$OUTPUT" 2>/dev/null |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
[[ -n $MAX_GLIBC ]] || fail "could not determine GLIBC requirement"
VERSION=${MAX_GLIBC#GLIBC_}
MAJOR=${VERSION%%.*}
MINOR=${VERSION#*.}; MINOR=${MINOR%%.*}
if ((MAJOR > 2 || (MAJOR == 2 && MINOR > 30))); then
  fail "$OUTPUT requires $MAX_GLIBC (maximum GLIBC_2.30)"
fi
if "$READELF" -dW "$OUTPUT" | grep -Eq '(RPATH|RUNPATH)'; then
  fail "public loader contains RPATH/RUNPATH"
fi
if "$READELF" -lW "$OUTPUT" | awk '$1 == "GNU_STACK" && $NF ~ /E/ {bad=1} END {exit !bad}'; then
  fail "public loader has an executable stack"
fi
NEEDED=$("$READELF" -dW "$OUTPUT" |
  awk -F'[][]' '/NEEDED/ {print $2}' | sort)
while IFS= read -r soname; do
  case $soname in
    libc.so.6|libdl.so.2|libgcc_s.so.1|libm.so.6|libpthread.so.0|\
    libatomic.so.1|libSDL2-2.0.so.0|libz.so.1) ;;
    *) fail "unexpected DT_NEEDED: $soname" ;;
  esac
done <<< "$NEEDED"
for required in libc.so.6 libSDL2-2.0.so.0 libz.so.1; do
  grep -Fx "$required" <<< "$NEEDED" >/dev/null ||
    fail "required DT_NEEDED is missing: $required"
done
if strings "$OUTPUT" | grep -Eq '/home/|/mnt/|192[.]168[.]'; then
  fail "public loader contains a private build path or test address"
fi

if [[ -n ${NXABI:-} ]]; then
  python3 -B "$NXABI" audit "$OUTPUT"
fi
printf 'ANGRYBIRDS UNIVERSAL BUILD OK: %s\n' "$OUTPUT"
printf 'glibc_max=%s interpreter=%s\n' "$MAX_GLIBC" "$INTERPRETER"
printf 'DT_NEEDED=%s\n' "$(tr '\n' ' ' <<< "$NEEDED")"
file "$OUTPUT"
sha256sum "$OUTPUT"
