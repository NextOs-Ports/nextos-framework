#!/usr/bin/env bash
# AArch64 build for Merchant of the Skies 2.0 (Unity 6 / Mali-450).
#
# Builds directly against the CURRENT NextOS toolchain sysroot and the glibc it
# ships (2.43 today, following future NextOS upgrades). The old low-glibc/Buster
# container rule is revoked and is deliberately NOT used here.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
OUTPUT=${MS_OUTPUT:-build/merchantskies-nextos}
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786233600}

fail() {
  printf 'merchantskies build error: %s\n' "$*" >&2
  exit 1
}

case $OUTPUT in
  /*|*../*|../*|*/..|..)
    fail "MS_OUTPUT must stay inside the port directory"
    ;;
esac

NEXTOS_ROOT=${NEXTOS_ROOT:-}
NXSR=${NEXTOS_SYSROOT:-}
if [[ -z $NXSR && -n $NEXTOS_ROOT ]]; then
  # Only a sysroot that actually carries the SDL2/EGL/GLES headers is usable;
  # the newest toolchain directory is not always the complete one.
  while IFS= read -r candidate; do
    [[ -d $candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/SDL2 ]] || continue
    NXSR=$candidate/aarch64-libreelec-linux-gnu/sysroot
  done < <(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V
  )
fi
[[ -n $NXSR ]] ||
  fail "set NEXTOS_SYSROOT to a sysroot containing SDL2/EGL/GLES headers"
[[ -d $NXSR/usr/include/SDL2 ]] ||
  fail "SDL2 headers are missing below NEXTOS_SYSROOT ($NXSR)"

for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-nm \
            aarch64-linux-gnu-readelf aarch64-linux-gnu-strip file strings; do
  command -v "$tool" >/dev/null 2>&1 || fail "tool is missing: $tool"
done

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
STRIP=aarch64-linux-gnu-strip

cd "$PORT_DIR"
mkdir -p -- "$(dirname -- "$OUTPUT")"

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf -- "$OBJDIR" "$STUBDIR"' EXIT INT TERM

mapfile -t SOURCES < <(find src -maxdepth 1 -type f -name '*.c' -print | sort)
[[ ${#SOURCES[@]} -gt 0 ]] || fail "no C sources found"
OBJS=()
for source in "${SOURCES[@]}"; do
  object=$OBJDIR/$(basename "${source%.c}").o
  "$CC" -std=gnu11 --sysroot="$NXSR" \
    -I src \
    -isystem "$NXSR/usr/include" \
    -isystem "$NXSR/usr/include/SDL2" \
    -O2 -fPIE -fno-strict-aliasing -fno-omit-frame-pointer \
    -ffile-prefix-map="$PORT_DIR"=. -fdebug-prefix-map="$PORT_DIR"=. \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# Record the firmware SDL SONAME without linking the real target library.
UNDEFINED=$(
  "$NM" --undefined-only "${OBJS[@]}" 2>/dev/null |
    awk '{print $NF}' | sort -u
)
: > "$STUBDIR/sdl.c"
while IFS= read -r symbol; do
  [[ $symbol == SDL_* ]] || continue
  printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/sdl.c"
done <<< "$UNDEFINED"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUBDIR/sdl.c" -o "$STUBDIR/libSDL2.so"

"$CC" --sysroot="$NXSR" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -Wl,--no-as-needed -lSDL2 -Wl,--as-needed \
  -L"$NXSR/usr/lib" -L"$NXSR/lib" \
  -Wl,-rpath-link,"$NXSR/usr/lib" -Wl,-rpath-link,"$NXSR/lib" \
  -ldl -lm -lpthread -lz -lgcc_s \
  -Wl,--build-id=sha1 -Wl,-z,relro,-z,now,-z,noexecstack
"$STRIP" --strip-debug "$OUTPUT"
chmod 0755 "$OUTPUT"

MACHINE=$("$READELF" -h "$OUTPUT" |
  sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[[ $MACHINE == AArch64 ]] || fail "unexpected ELF machine: $MACHINE"
INTERPRETER=$("$READELF" -lW "$OUTPUT" |
  sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ $INTERPRETER == /lib/ld-linux-aarch64.so.1 ]] ||
  fail "unexpected PT_INTERP: $INTERPRETER"

printf 'merchantskies: built %s\n' "$OUTPUT"
"$READELF" -dW "$OUTPUT" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/  NEEDED \1/p'
