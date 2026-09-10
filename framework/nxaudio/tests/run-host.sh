#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source_root="$repo_root/framework/nxaudio"
work_root=$(mktemp -d /tmp/nxaudio-m14-host.XXXXXX)
trap 'find "$work_root" -depth -delete' EXIT HUP INT TERM

for compiler in gcc clang; do
  command -v "$compiler" >/dev/null
  build="$work_root/$compiler-build"
  prefix="$work_root/$compiler-prefix"
  cmake -S "$source_root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$compiler" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_C_FLAGS='-Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
  cmake --build "$build" -j2
  if nm -u "$build/test-nxaudio" | grep -Eq 'SDL_|snd_|pulse|pipewire|alc?Open|FMOD|Wwise'; then
    echo "nxaudio host fixture imports a real audio provider" >&2
    exit 1
  fi
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir "$build" --output-on-failure
  cmake --install "$build"
  test -f "$prefix/include/nxaudio.h"
  test -f "$prefix/lib/libnxaudio.a"
done

mkdir -p "$work_root/analyze"
(
  cd "$work_root/analyze"
  clang --analyze -std=c11 -D_POSIX_C_SOURCE=200809L \
    -I"$source_root/include" "$source_root/src/nxaudio.c"
)

echo "nxaudio_m14_host=PASS gcc=1 clang=1 asan=1 ubsan=1 lsan=1 analyze=1"
echo "guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0"
