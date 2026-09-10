#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Hermetic M12 host gate: no guest, device, GPU, controller or real audio open.
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
work_root=$(mktemp -d /tmp/nxcompat-host.XXXXXX)

cleanup() {
  local status=$?
  trap - EXIT
  if [[ $work_root == /tmp/nxcompat-host.* && -d $work_root ]]; then
    find "$work_root" -depth -delete
  fi
  exit "$status"
}
trap cleanup EXIT

export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy
export XDG_RUNTIME_DIR=$work_root/runtime
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
mkdir -p -- "$XDG_RUNTIME_DIR"

cmake_flags=(
  -DCMAKE_BUILD_TYPE=Debug
  "-DCMAKE_C_FLAGS=-std=c11 -Werror -fsanitize=address,undefined -fno-omit-frame-pointer"
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
)

for compiler in gcc clang; do
  build=$work_root/nxcompat-$compiler
  install_root=$work_root/install-$compiler
  CC=$compiler cmake -S "$repo_root/framework/nxcompat" -B "$build" \
    "${cmake_flags[@]}" \
    -DNXCOMPAT_BUILD_TESTS=ON \
    -DNXCOMPAT_BUILD_TOOLS=ON \
    -DNXCOMPAT_WITH_SDL2=ON \
    -DCMAKE_INSTALL_PREFIX="$install_root"
  cmake --build "$build" -j2
  if nm "$build/libnxcompat.a" "$build/libnxcompat-sdl2.a" |
      grep -q 'nxcompat_test_'; then
    printf 'public library contains a test injection symbol\n' >&2
    exit 1
  fi
  if nm -u "$build/test-nxcompat-sdl2" |
      grep -Eq 'SDL_(OpenAudioDevice|CloseAudioDevice|GetNumAudioDevices)'; then
    printf 'sealed SDL test can reach a real audio device API\n' >&2
    exit 1
  fi
  ctest --test-dir "$build" --output-on-failure
  cmake --install "$build"
done

for compiler in gcc clang; do
  build=$work_root/nxgl-$compiler
  install_root=$work_root/install-$compiler
  CC=$compiler cmake -S "$repo_root/framework/nxgl" -B "$build" \
    "${cmake_flags[@]}" \
    -DNXGL_BUILD_TESTS=ON \
    -DNXGL_BUILD_NATIVE_TESTS=OFF \
    -DNXGL_WITH_NXCOMPAT=ON \
    -DCMAKE_INSTALL_PREFIX="$install_root"
  cmake --build "$build" -j2
  if nm -u "$build/test-nxgl-nxcompat" |
      grep -Eq 'SDL_(InitSubSystem|CreateWindow|GL_CreateContext)'; then
    printf 'nxgl fake bridge can open a native graphics subsystem\n' >&2
    exit 1
  fi
  if nm -u "$build/test-nxgl-environment" |
      grep -Eq 'SDL_|egl|gl[A-Z]'; then
    printf 'nxgl environment fixture can reach a graphics API\n' >&2
    exit 1
  fi
  if readelf -d "$build/test-nxgl-environment" |
      grep -Eq 'NEEDED.*(SDL|EGL|GLES)'; then
    printf 'nxgl environment fixture links a graphics provider\n' >&2
    exit 1
  fi
  ctest --test-dir "$build" --output-on-failure \
    -R '^nxgl-(environment|nxcompat)$'
  cmake --install "$build"
done

for compiler in gcc clang; do
  build=$work_root/nxinput-$compiler
  install_root=$work_root/install-$compiler
  CC=$compiler cmake -S "$repo_root/framework/nxinput" -B "$build" \
    "${cmake_flags[@]}" \
    -DNXINPUT_BUILD_TESTS=ON \
    -DNXINPUT_BUILD_NATIVE_TESTS=OFF \
    -DNXINPUT_WITH_NXCOMPAT=ON \
    -DCMAKE_INSTALL_PREFIX="$install_root"
  cmake --build "$build" -j2
  if nm -u "$build/test-nxinput-nxcompat" |
      grep -Eq 'SDL_(NumJoysticks|GameControllerOpen|JoystickOpen)'; then
    printf 'nxinput fake bridge can enumerate a native controller\n' >&2
    exit 1
  fi
  ctest --test-dir "$build" --output-on-failure \
    -R '^(nxinput-static-gate|nxinput-nxcompat|nxinput-evdev-chord)$'
  cmake --install "$build"
done

for compiler in gcc clang; do
  install_root=$work_root/install-$compiler
  for relative in \
      lib/libnxcompat.a lib/libnxcompat-sdl2.a \
      lib/libnxgl.a lib/libnxgl-nxcompat.a \
      lib/libnxinput.a lib/libnxinput-nxcompat.a \
      include/nxcompat.h include/nxcompat_sdl2.h \
      include/nxgl.h include/nxgl_nxcompat.h \
      include/nxinput.h include/nxinput_nxcompat.h \
      share/nxcompat/capabilities-v1.json bin/nxcompat-probe; do
    test -f "$install_root/$relative"
  done
  cmp -s "$repo_root/framework/nxcompat/capabilities-v1.json" \
    "$install_root/share/nxcompat/capabilities-v1.json"
  if find "$install_root" -type f -name '*test*' -print -quit |
      grep -q .; then
    printf 'test-only artifact escaped into the install prefix\n' >&2
    exit 1
  fi
  if nm "$install_root"/lib/*.a | grep -q 'nxcompat_test_'; then
    printf 'installed public archive contains a test injection symbol\n' >&2
    exit 1
  fi
  "$compiler" -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    $(pkg-config --cflags sdl2 egl glesv2) \
    -I"$install_root/include" \
    "$repo_root/framework/nxcompat/tests/test_install_smoke.c" \
    -L"$install_root/lib" \
    -lnxgl-nxcompat -lnxgl -lnxinput-nxcompat -lnxinput \
    -lnxcompat-sdl2 -lnxcompat \
    $(pkg-config --libs sdl2 egl glesv2) -lm \
    -o "$work_root/install-smoke-$compiler"
done

mkdir -p -- "$work_root/analyzer-clang"
(
  cd -- "$work_root/analyzer-clang"
  clang --analyze -std=c11 -D_DEFAULT_SOURCE \
    -I"$repo_root/framework/nxcompat/include" \
    -I"$repo_root/framework/nxcompat/src" \
    "$repo_root/framework/nxcompat/src/nxcompat.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_backend.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_graphics.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_plan.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_probe.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_receipts.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_registry.c" \
    "$repo_root/framework/nxcompat/src/nxcompat_report.c"
)

for source in "$repo_root"/framework/nxcompat/src/*.c; do
  gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fanalyzer \
    -I"$repo_root/framework/nxcompat/include" \
    -I"$repo_root/framework/nxcompat/src" -fsyntax-only "$source"
done

[[ ${SDL_AUDIODRIVER:-} == dummy ]]
[[ ${SDL_VIDEODRIVER:-} == dummy ]]
printf 'nxcompat_host=PASS gcc=1 clang=1 asan=1 ubsan=1 lsan=1 '
printf 'analyze=1 fake_graphics=1 fake_audio=1 fake_input=1 '
printf 'guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0\n'
