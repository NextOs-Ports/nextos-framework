#!/bin/bash
# Build the public i386 guest loader with Debian Buster (GLIBC <= 2.30).
#
# Run from the host with a header sysroot containing usr/include/{SDL2,EGL,GLES2}:
#   docker run --rm \
#     -v "$PWD":/repo -v "$TARGET_HEADER_SYSROOT":/sysroot:ro \
#     debian:buster bash /repo/build_buster_x86_box32.sh
set -euo pipefail

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

CC=${CC:-gcc}
NM=${NM:-nm}
READELF=${READELF:-readelf}
STRIP=${STRIP:-strip}
REPO=${ASM2_REPO:-/repo}
HEADER_ROOT=${ASM2_HEADER_ROOT:-/sysroot/usr/include}

if ! command -v "$CC" >/dev/null 2>&1 ||
   ! printf 'int main(void){return 0;}\n' |
     "$CC" -m32 -x c - -o /tmp/asm2-i386-compiler-probe 2>/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf 'deb http://archive.debian.org/debian buster main\n' > /etc/apt/sources.list
  printf 'deb http://archive.debian.org/debian-security buster/updates main\n' >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq gcc-multilib binutils file >/dev/null
fi
rm -f -- /tmp/asm2-i386-compiler-probe

for tool in "$CC" "$NM" "$READELF" "$STRIP" file sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || {
    printf 'required build tool not found: %s\n' "$tool" >&2
    exit 1
  }
done
[ -f "$HEADER_ROOT/SDL2/SDL.h" ] || {
  printf 'SDL2 headers not found below: %s\n' "$HEADER_ROOT" >&2
  exit 1
}

cd "$REPO"
sources=(
  src/main.c src/imports.c src/so_util_x86.c src/util.c src/error.c
  src/bionic_compat.c src/platform_shims.c src/opensl_bridge.c
  src/pthread_bridge.c src/jni_bridge.c src/android_callbacks.c
  src/installer_compat.c src/startup_compat.c src/audio_compat.c
  src/shop_compat.c src/video.c src/first_accept.c src/input.c
  src/x86_runtime_compat.c src/capture_x86.c
  src/setjmp_bridge_x86.S src/x86_gl_lazy.S
)

object_dir=$(mktemp -d)
stub_dir=$(mktemp -d)
cleanup() {
  rm -rf -- "$object_dir" "$stub_dir"
}
trap cleanup EXIT INT TERM

objects=()
for source in "${sources[@]}"; do
  object="$object_dir/$(basename "$source").o"
  "$CC" -m32 -std=gnu11 -O2 -fno-pie -D_GNU_SOURCE=1 \
    -Wall -Wextra -Werror -I src \
    -idirafter "$HEADER_ROOT/SDL2" -idirafter "$HEADER_ROOT" \
    -c "$source" -o "$object"
  objects+=("$object")
done

undefined=$(
  "$NM" --undefined-only "${objects[@]}" 2>/dev/null |
    awk '{print $NF}' | sort -u
)
printf '%s\n' "$undefined" | grep -E '^SDL_' |
  sed 's/.*/void &(void){}/' > "$stub_dir/sdl.c"
"$CC" -m32 -shared -fPIC -nostdlib \
  -Wl,-soname,libSDL2-2.0.so.0 \
  "$stub_dir/sdl.c" -o "$stub_dir/libSDL2.so"

"$CC" -m32 -no-pie -static-libgcc \
  -Wl,--export-dynamic -Wl,--no-as-needed -Wl,--allow-shlib-undefined \
  "${objects[@]}" -L"$stub_dir" -lSDL2 -ldl -lm -pthread \
  -o asm2_127_x86_box32
"$STRIP" --strip-unneeded asm2_127_x86_box32

newest=$(
  "$READELF" --version-info asm2_127_x86_box32 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1
)
maximum=$(printf '%s\n%s\n' GLIBC_2.30 "$newest" | sort -V | tail -1)
[ "$maximum" = GLIBC_2.30 ] || {
  printf 'i386 loader exceeds public ABI ceiling: %s\n' "$newest" >&2
  exit 1
}

file asm2_127_x86_box32
printf 'glibc max = %s\n' "$newest"
sha256sum asm2_127_x86_box32
"$READELF" -d asm2_127_x86_box32 | grep NEEDED
