#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
RUN_SCRIPT=${1:-"$TEST_DIR/../../package/sources/run.sh"}
OUTER_SCRIPT=${2:-"$TEST_DIR/../../package/sources/The Amazing Spider-Man 2.sh"}

fail() {
  printf 'muos_armhf_audio_env_test: FAIL: %s\n' "$*" >&2
  exit 1
}

load_function() {
  local function_name=$1
  # shellcheck disable=SC1090
  source <(
    awk -v signature="${function_name}()" '
      $0 == signature " {" { emit=1 }
      emit { print }
      emit && /^}/ { exit }
    ' "$RUN_SCRIPT"
  )
}

grep -Eq '^[[:space:]]*PORT_32BIT="Y"[[:space:]]*$' "$OUTER_SCRIPT" ||
  fail "outer launcher lacks the literal muOS 32-bit marker"

load_function platform_path
load_function configure_arm_audio_runtime
declare -F platform_path >/dev/null ||
  fail "platform_path was not loaded"
declare -F configure_arm_audio_runtime >/dev/null ||
  fail "configure_arm_audio_runtime was not loaded"

fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p \
  "$fixture/usr/lib32/pipewire-0.3" \
  "$fixture/usr/lib32/spa-0.2" \
  "$fixture/usr/lib32/alsa-lib"
touch "$fixture/usr/lib32/alsa-lib/libasound_module_pcm_pipewire.so"

PLATFORM_KIND=armhf
unset PORT_32BIT PIPEWIRE_MODULE_DIR SPA_PLUGIN_DIR ALSA_PLUGIN_DIR
configure_arm_audio_runtime "$fixture" >/dev/null

[ "${PORT_32BIT:-}" = Y ] ||
  fail "ARMHF route did not export PORT_32BIT"
[ "${PIPEWIRE_MODULE_DIR:-}" = "$fixture/usr/lib32/pipewire-0.3" ] ||
  fail "ARMHF PipeWire module directory was not selected"
[ "${SPA_PLUGIN_DIR:-}" = "$fixture/usr/lib32/spa-0.2" ] ||
  fail "ARMHF SPA plugin directory was not selected"
[ "${ALSA_PLUGIN_DIR:-}" = "$fixture/usr/lib32/alsa-lib" ] ||
  fail "ARMHF ALSA plugin directory was not selected"

PLATFORM_KIND=x5m-box32
PORT_32BIT=sentinel
PIPEWIRE_MODULE_DIR=sentinel-pipewire
SPA_PLUGIN_DIR=sentinel-spa
ALSA_PLUGIN_DIR=sentinel-alsa
configure_arm_audio_runtime "$fixture" >/dev/null

[ "$PORT_32BIT" = sentinel ] &&
  [ "$PIPEWIRE_MODULE_DIR" = sentinel-pipewire ] &&
  [ "$SPA_PLUGIN_DIR" = sentinel-spa ] &&
  [ "$ALSA_PLUGIN_DIR" = sentinel-alsa ] ||
  fail "non-ARMHF route was modified"

printf 'muos_armhf_audio_env_test: PASS\n'
