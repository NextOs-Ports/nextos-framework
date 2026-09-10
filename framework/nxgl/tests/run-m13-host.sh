#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Hermetic M13 host gate: injected providers only; no display or device proof.
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
work_root=$(mktemp -d /tmp/nxgl-m13-host.XXXXXX)

cleanup() {
  local status=$?
  trap - EXIT
  if [[ $work_root == /tmp/nxgl-m13-host.* && -d $work_root ]]; then
    find "$work_root" -depth -delete
  fi
  exit "$status"
}
trap cleanup EXIT

export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy
export XDG_RUNTIME_DIR=$work_root/runtime
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export LSAN_OPTIONS=exitcode=23
export UBSAN_OPTIONS=halt_on_error=1
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
mkdir -p -- "$XDG_RUNTIME_DIR"

strict_flags=(
  -std=c99
  -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)
cmake_flags=(
  -DCMAKE_BUILD_TYPE=Debug
  "-DCMAKE_C_FLAGS=-std=c99 -Werror -fsanitize=address,undefined -fno-omit-frame-pointer"
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
  -DNXGL_BUILD_TESTS=ON
  -DNXGL_BUILD_NATIVE_TESTS=OFF
  -DNXGL_ENABLE_SANITIZERS=ON
  -DNXGL_WITH_NXCOMPAT=OFF
  -DCMAKE_INSTALL_LIBDIR=lib
)
m13_executables=(
  test-nxgl-open-v2
  test-nxgl-present-v2
  test-nxgl-metrics
  test-nxgl-diagnostics
  test-nxgl-sdl-hint
  test-nxgl-provider-recovery
)
m13_provider_targets=(
  nxgl-test-provider-good
  nxgl-test-provider-dependency
  nxgl-test-provider-mixed
)

seal_fake_executable() {
  local executable=$1
  test -x "$executable"
  if nm -u "$executable" |
      grep -Eq ' U (SDL_|egl[A-Za-z0-9_]*|gl[A-Z][A-Za-z0-9_]*|drm[A-Za-z0-9_]*|gbm[A-Za-z0-9_]*|udev[A-Za-z0-9_]*|wl_display[A-Za-z0-9_]*|X(OpenDisplay|CreateWindow)|socket(@|$)|connect(@|$)|getaddrinfo(@|$))'; then
    printf 'sealed M13 fixture reaches a graphics/device/network API: %s\n' \
      "$executable" >&2
    return 1
  fi
  if readelf -d "$executable" |
      grep -Eq 'NEEDED.*(libSDL|libEGL|libGLES|libGL\.|libGLX|libdrm|libgbm|libudev|libwayland|libX11)'; then
    printf 'sealed M13 fixture links a graphics provider: %s\n' \
      "$executable" >&2
    return 1
  fi
  if strings "$executable" |
      grep -Eq '/dev/(dri|fb[0-9]*|input|snd)|/sys/class/(drm|graphics|input)'; then
    printf 'sealed M13 fixture embeds a physical-device path: %s\n' \
      "$executable" >&2
    return 1
  fi
}

check_provider_recovery_archive() {
  local archive=$1
  test -f "$archive"
  if nm -g --defined-only "$archive" |
      grep -Eq '(nxgl_test_|fake_|test_nxgl)'; then
    printf 'public provider-recovery archive contains a test-only symbol: %s\n' \
      "$archive" >&2
    return 1
  fi
  for symbol in nxgl_probe_sdl_provider_v2 \
      nxgl_reexec_sdl_provider_pair_v2 \
      nxgl_provider_recovery_reason_name_v2; do
    if ! nm -g --defined-only "$archive" |
        awk '{print $NF}' | grep -Fxq "$symbol"; then
      printf 'public provider-recovery archive is missing %s\n' "$symbol" >&2
      return 1
    fi
  done
}

check_public_archive() {
  local archive=$1
  test -f "$archive"
  if nm -g --defined-only "$archive" |
      grep -Eq '(nxgl_test_|fake_|test_nxgl)'; then
    printf 'public nxgl archive contains a test-only symbol: %s\n' \
      "$archive" >&2
    return 1
  fi
  for symbol in nxgl_open_v2 nxgl_present_v2 \
      nxgl_calculate_surface_metrics_v2 nxgl_surface_observe_v2 \
      nxgl_classify_black_silhouette_v2 nxgl_plan_sdl_provider_pair \
      nxgl_sdl_precontext_recovery_v2_init \
      nxgl_plan_sdl_precontext_recovery_v2 \
      nxgl_sdl_video_hint_options_v2_init \
      nxgl_sdl_video_hint_receipt_v2_init \
      nxgl_sanitize_sdl_video_hint_v2 \
      nxgl_complete_sdl_video_hint_receipt_v2 \
      nxgl_sdl_video_hint_action_name_v2 \
      nxgl_format_sdl_video_hint_receipt_v2; do
    if ! nm -g --defined-only "$archive" |
        awk '{print $NF}' | grep -Fxq "$symbol"; then
      printf 'public nxgl archive is missing M13 symbol %s\n' "$symbol" >&2
      return 1
    fi
  done
}

link_install_smoke() {
  local compiler=$1
  local install_root=$2
  local output=$3
  "$compiler" "${strict_flags[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    $(pkg-config --cflags sdl2) \
    -I"$install_root/include" -x c - \
    -L"$install_root/lib" -Wl,-z,defs -lnxgl-provider-recovery -lnxgl \
    $(pkg-config --libs sdl2) -ldl -lm -fsanitize=address,undefined \
    -o "$output" <<'EOF'
/* Link-only installed API smoke.  The gate must never execute this ELF. */
#include "nxgl.h"
#include "nxgl_provider_recovery.h"

static int (*volatile open_v2_fn)(const nxgl_open_options_v2 *,
                                  nxgl_context **, nxgl_report_v2 *) =
    nxgl_open_v2;
static int (*volatile close_v2_fn)(nxgl_context *) = nxgl_close_v2;
static int (*volatile present_v2_fn)(nxgl_context *,
                                     const nxgl_present_policy_v2 *,
                                     nxgl_present_result_v2 *) =
    nxgl_present_v2;
static int (*volatile metrics_v2_fn)(
    const nxgl_surface_metrics_input_v2 *, nxgl_surface_metrics_v2 *) =
    nxgl_calculate_surface_metrics_v2;
static int (*volatile observe_v2_fn)(
    nxgl_surface_state_v2 *, const nxgl_surface_observation_v2 *) =
    nxgl_surface_observe_v2;
static int (*volatile silhouette_v2_fn)(
    const nxgl_silhouette_observation_v2 *,
    nxgl_silhouette_diagnosis_v2 *) = nxgl_classify_black_silhouette_v2;
static nxgl_sdl_provider_pair_plan (*volatile provider_pair_fn)(
    const char *, const char *, const char *, int, int, int, int, int) =
    nxgl_plan_sdl_provider_pair;
static void (*volatile precontext_init_fn)(
    nxgl_sdl_precontext_recovery_v2 *) =
    nxgl_sdl_precontext_recovery_v2_init;
static nxgl_sdl_provider_pair_plan (*volatile precontext_plan_fn)(
    const nxgl_sdl_precontext_recovery_v2 *) =
    nxgl_plan_sdl_precontext_recovery_v2;
static void (*volatile sdl_hint_options_init_fn)(
    nxgl_sdl_video_hint_options_v2 *) =
    nxgl_sdl_video_hint_options_v2_init;
static void (*volatile sdl_hint_receipt_init_fn)(
    nxgl_sdl_video_hint_receipt_v2 *) =
    nxgl_sdl_video_hint_receipt_v2_init;
static int (*volatile sdl_hint_sanitize_fn)(
    const nxgl_sdl_video_hint_options_v2 *,
    nxgl_sdl_video_hint_receipt_v2 *) =
    nxgl_sanitize_sdl_video_hint_v2;
static int (*volatile sdl_hint_complete_fn)(
    nxgl_sdl_video_hint_receipt_v2 *) =
    nxgl_complete_sdl_video_hint_receipt_v2;
static const char *(*volatile sdl_hint_action_name_fn)(
    nxgl_sdl_video_hint_action_v2) =
    nxgl_sdl_video_hint_action_name_v2;
static int (*volatile sdl_hint_format_fn)(
    const nxgl_sdl_video_hint_receipt_v2 *, char *, size_t) =
    nxgl_format_sdl_video_hint_receipt_v2;
static int (*volatile provider_probe_fn)(
    const nxgl_sdl_provider_probe_options_v2 *,
    nxgl_sdl_provider_probe_receipt_v2 *) = nxgl_probe_sdl_provider_v2;
static int (*volatile provider_reexec_fn)(
    const nxgl_sdl_provider_reexec_options_v2 *,
    nxgl_sdl_provider_reexec_result_v2 *) =
    nxgl_reexec_sdl_provider_pair_v2;

int main(void) {
  return open_v2_fn && close_v2_fn && present_v2_fn && metrics_v2_fn &&
                 observe_v2_fn && silhouette_v2_fn && provider_pair_fn &&
                 precontext_init_fn && precontext_plan_fn &&
                 sdl_hint_options_init_fn && sdl_hint_receipt_init_fn &&
                 sdl_hint_sanitize_fn && sdl_hint_complete_fn &&
                 sdl_hint_action_name_fn && sdl_hint_format_fn &&
                 provider_probe_fn && provider_reexec_fn
             ? 0
             : 1;
}
EOF
  test -x "$output"
}

for compiler in gcc clang; do
  build=$work_root/build-$compiler
  install_root=$work_root/install-$compiler
  CC=$compiler cmake -S "$repo_root/framework/nxgl" -B "$build" \
    "${cmake_flags[@]}" -DCMAKE_INSTALL_PREFIX="$install_root"
  # Build only the public archive and sealed M13 fixtures.  M12 owns its
  # environment/bridge targets and executes them in its existing host gate.
  cmake --build "$build" --target nxgl nxgl-provider-recovery \
    "${m13_provider_targets[@]}" "${m13_executables[@]}" -j2

  check_public_archive "$build/libnxgl.a"
  check_provider_recovery_archive "$build/libnxgl-provider-recovery.a"
  for executable in "${m13_executables[@]}"; do
    seal_fake_executable "$build/$executable"
  done

  # The symbol/DT_NEEDED/device-path seals above are deliberately before the
  # first test execution. The regex selects only the six hermetic M13 tests.
  ctest --test-dir "$build" --output-on-failure \
    -R '^nxgl-m13-(open-v2|present-v2|metrics|diagnostics|sdl-hint|provider-recovery)$'

  cmake --install "$build"
  test -f "$install_root/lib/libnxgl.a"
  test -f "$install_root/lib/libnxgl-provider-recovery.a"
  test -f "$install_root/include/nxgl.h"
  test -f "$install_root/include/nxgl_provider_recovery.h"
  if find "$install_root" -type f -name '*test*' -print -quit |
      grep -q .; then
    printf 'test-only artifact escaped into the M13 install prefix\n' >&2
    exit 1
  fi
  check_public_archive "$install_root/lib/libnxgl.a"
  check_provider_recovery_archive \
    "$install_root/lib/libnxgl-provider-recovery.a"
  link_install_smoke "$compiler" "$install_root" \
    "$work_root/install-link-only-$compiler"
done

analyzer_root=$work_root/analyzers
mkdir -p -- "$analyzer_root/clang" "$analyzer_root/gcc"
sdl_cflags=($(pkg-config --cflags sdl2))
(
  cd -- "$analyzer_root/clang"
  # Analyze the installed/default SDL path as source only.  Static analysis
  # neither links nor executes a provider, but it prevents fake-only coverage
  # from hiding defects in the production translation units.
  clang --analyze -Xanalyzer -analyzer-werror "${strict_flags[@]}" \
    -D_POSIX_C_SOURCE=200809L "${sdl_cflags[@]}" \
    -I"$repo_root/framework/nxgl/include" \
    -I"$repo_root/framework/nxgl/src" \
    "$repo_root/framework/nxgl/src/nxgl_arbiter.c" \
    "$repo_root/framework/nxgl/src/nxgl_logic.c" \
    "$repo_root/framework/nxgl/src/nxgl_sdl2.c" \
    "$repo_root/framework/nxgl/src/nxgl_present.c" \
    "$repo_root/framework/nxgl/src/nxgl_metrics.c" \
    "$repo_root/framework/nxgl/src/nxgl_diagnostics.c" \
    "$repo_root/framework/nxgl/src/nxgl_sdl_hint.c" \
    "$repo_root/framework/nxgl/src/nxgl_provider_recovery.c"
  clang --analyze -Xanalyzer -analyzer-werror "${strict_flags[@]}" \
    -D_POSIX_C_SOURCE=200809L \
    -DNXGL_M13_TESTING=1 "${sdl_cflags[@]}" \
    -I"$repo_root/framework/nxgl/include" \
    -I"$repo_root/framework/nxgl/src" \
    "$repo_root/framework/nxgl/src/nxgl_logic.c" \
    "$repo_root/framework/nxgl/src/nxgl_sdl2.c" \
    "$repo_root/framework/nxgl/tests/test_nxgl_open_v2.c"
  clang --analyze -Xanalyzer -analyzer-werror "${strict_flags[@]}" \
    -D_POSIX_C_SOURCE=200809L \
    -DNXGL_PRESENT_V2_TESTING=1 "${sdl_cflags[@]}" \
    -I"$repo_root/framework/nxgl/include" \
    -I"$repo_root/framework/nxgl/src" \
    "$repo_root/framework/nxgl/src/nxgl_present.c" \
    "$repo_root/framework/nxgl/tests/test_nxgl_present_v2.c"
  clang --analyze -Xanalyzer -analyzer-werror "${strict_flags[@]}" \
    -D_POSIX_C_SOURCE=200809L \
    "${sdl_cflags[@]}" -I"$repo_root/framework/nxgl/include" \
    "$repo_root/framework/nxgl/src/nxgl_metrics.c" \
    "$repo_root/framework/nxgl/tests/test_nxgl_metrics.c" \
    "$repo_root/framework/nxgl/src/nxgl_diagnostics.c" \
    "$repo_root/framework/nxgl/tests/test_nxgl_diagnostics.c"
  clang --analyze -Xanalyzer -analyzer-werror "${strict_flags[@]}" \
    -D_POSIX_C_SOURCE=200809L \
    -DNXGL_SDL_HINT_TESTING=1 "${sdl_cflags[@]}" \
    -I"$repo_root/framework/nxgl/include" \
    -I"$repo_root/framework/nxgl/src" \
    "$repo_root/framework/nxgl/src/nxgl_arbiter.c" \
    "$repo_root/framework/nxgl/src/nxgl_sdl_hint.c" \
    "$repo_root/framework/nxgl/tests/test_nxgl_sdl_hint.c"
)

while IFS='|' read -r definition source; do
  definitions=()
  if [[ -n $definition ]]; then
    definitions+=("-D$definition")
  fi
  (
    cd -- "$analyzer_root/gcc"
    gcc -fanalyzer -fsyntax-only "${strict_flags[@]}" \
      -D_POSIX_C_SOURCE=200809L "${definitions[@]}" "${sdl_cflags[@]}" \
      -I"$repo_root/framework/nxgl/include" \
      -I"$repo_root/framework/nxgl/src" "$repo_root/$source"
  )
done <<'EOF'
|framework/nxgl/src/nxgl_arbiter.c
|framework/nxgl/src/nxgl_logic.c
|framework/nxgl/src/nxgl_sdl2.c
|framework/nxgl/src/nxgl_present.c
NXGL_M13_TESTING=1|framework/nxgl/src/nxgl_logic.c
NXGL_M13_TESTING=1|framework/nxgl/src/nxgl_sdl2.c
NXGL_M13_TESTING=1|framework/nxgl/tests/test_nxgl_open_v2.c
NXGL_PRESENT_V2_TESTING=1|framework/nxgl/src/nxgl_present.c
NXGL_PRESENT_V2_TESTING=1|framework/nxgl/tests/test_nxgl_present_v2.c
|framework/nxgl/src/nxgl_metrics.c
|framework/nxgl/tests/test_nxgl_metrics.c
|framework/nxgl/src/nxgl_diagnostics.c
|framework/nxgl/tests/test_nxgl_diagnostics.c
|framework/nxgl/src/nxgl_sdl_hint.c
NXGL_SDL_HINT_TESTING=1|framework/nxgl/tests/test_nxgl_sdl_hint.c
|framework/nxgl/src/nxgl_provider_recovery.c
NXGL_PROVIDER_RECOVERY_TESTING=1|framework/nxgl/tests/test_nxgl_provider_recovery.c
EOF

[[ ${SDL_AUDIODRIVER:-} == dummy ]]
[[ ${SDL_VIDEODRIVER:-} == dummy ]]
[[ -z ${DISPLAY:-} && -z ${WAYLAND_DISPLAY:-} ]]
printf 'nxgl_m13_host=PASS gcc=1 clang=1 asan=1 ubsan=1 lsan=1 '
printf 'analyze=1 fake_open=1 fake_present=1 metrics=1 diagnostics=1 sdl_hint=1 provider_recovery=1 '
printf 'install_link_only=1 public_test_symbols=0 sdl_runtime_calls=0 '
printf 'sdl_video_initialized=0 window_created=0 context_created=0 '
printf 'real_gpu_or_display_opened=0 real_egl_or_gles_driver_opened=0 '
printf 'guest_code_executed=0 external_guest_code_executed=0 hardware_ran=0 '
printf 'physical_device_evidence=0 device_access=0 network_access=0 '
printf 'session_access=0\n'
