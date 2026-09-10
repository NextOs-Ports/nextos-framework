#!/usr/bin/env bash
# Verify that the public package keeps its two native X5M SDL roles separate.
set -euo pipefail

RUN_SH=${1:-}
OUTER_SH=${2:-}
ENV_SH=${3:-}

fail() {
  printf 'x5m extractor SDL scope test: FAIL: %s\n' "$*" >&2
  exit 1
}

[ -f "$RUN_SH" ] ||
  fail "usage: $0 PATH/TO/package/sources/run.sh PATH/TO/outer-launcher.sh"
[ -f "$OUTER_SH" ] ||
  fail "usage: $0 RUN_SH OUTER_SH [X5M_ENV_SH]"
[ -n "$ENV_SH" ] || ENV_SH=$(dirname -- "$RUN_SH")/x5m-runtime-env.sh
[ -f "$ENV_SH" ] ||
  fail "X5M process-scope helper was not found: $ENV_SH"

bash -n "$RUN_SH"
bash -n "$OUTER_SH"
bash -n "$ENV_SH"

fixture=$(mktemp -d "${TMPDIR:-/tmp}/asm2-x5m-sdl-scope.XXXXXX")
cleanup() {
  rm -rf -- "$fixture"
}
trap cleanup EXIT INT TERM

awk '
  /^configure_x5m_runtime[(][)] \{/ { inside=1 }
  inside { print }
  inside && /^}/ { exit }
' "$RUN_SH" > "$fixture/configure_x5m_runtime.sh"

[ -s "$fixture/configure_x5m_runtime.sh" ] ||
  fail "configure_x5m_runtime was not found"
if grep -Eq '(^|[[:space:]])(export[[:space:]]+)?LD_LIBRARY_PATH=' \
    "$fixture/configure_x5m_runtime.sh"; then
  fail "game-only sdl2-compat leaked into the extractor environment"
fi

game_scope='LD_LIBRARY_PATH="$native_lib_dir:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \'
extractor_scope='LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$control_folder/libs:$control_folder/libs.aarch64" \'

[ "$(grep -Fxc "    $game_scope" "$ENV_SH")" -eq 1 ] ||
  fail "the Box32 game invocation does not have one private sdl2-compat scope"
[ "$(grep -Fxc "  $extractor_scope" "$ENV_SH")" -eq 1 ] ||
  fail "the X5M extractor does not have one firmware SDL2/KMSDRM scope"

grep -Fq '"$box32_bin" ./asm2_127_x86_box32 "$game_dir"' "$ENV_SH" ||
  fail "the scoped Box32 game invocation is missing"
grep -Fq '"$game_dir/run-extractor.sh" --abi x86' "$ENV_SH" ||
  fail "the native extractor invocation does not select the x86 owner-data ABI"
grep -Fq 'asm2_x5m_run_extractor "$GAMEDIR" "$CONTROLFOLDER"' "$RUN_SH" ||
  fail "the public launcher does not call the versioned extractor helper"
grep -Fq \
  'asm2_x5m_run_game "$GAMEDIR" "$X5_NATIVE_LIB_DIR" "$X5_BOX32_BIN"' \
  "$RUN_SH" ||
  fail "the public launcher does not call the versioned game helper"

if grep -Eq '^[[:space:]]*export[[:space:]]+ASM2_EXTRACTOR_ONLY=' \
    "$OUTER_SH" "$RUN_SH" "$ENV_SH"; then
  fail "the maintainer-only extractor gate is enabled by a public launcher"
fi

printf 'x5m extractor SDL scope test: PASS\n'
