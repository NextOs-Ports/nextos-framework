#!/usr/bin/env bash
# Process-free release gate for the Bomb Chicken universal integration.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$PORT_DIR/../.." && pwd -P)

fail() {
  printf 'bombchicken host gate error: %s\n' "$*" >&2
  exit 1
}

for tool in bash chmod clang cmp cp docker file gcc git grep ln mkdir mktemp mv \
            python3 readelf rm sha256sum stat strings; do
  command -v "$tool" >/dev/null 2>&1 || fail "required host tool missing: $tool"
done

WORK_ROOT=$(mktemp -d "$PORT_DIR/.host-gate.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "$PORT_DIR"/.host-gate.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *)
      printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2
      ;;
  esac
}
trap cleanup EXIT INT TERM
REL_WORK=${WORK_ROOT#"$PORT_DIR"/}

bash -n "$PORT_DIR/build.sh"
bash -n "$PORT_DIR/nxextract/nxextract-runtime-env.sh"
bash -n "$PORT_DIR/nxextract/run-extractor.sh"
# 0.6.3: single self-contained launcher; the 0.5.1 library, its migration
# helper and the overlay test are retired.
if [[ ${BC_PREFREEZE:-0} != 1 ]]; then
  bash -n "$PORT_DIR/Bomb Chicken.sh"
fi

python3 -B "$PORT_DIR/nxextract/nxextract.py" recipe-check \
  --recipe "$PORT_DIR/extractor.json"

SANITIZER_FLAGS=(
  -std=c11 -D_POSIX_C_SOURCE=200809L
  -I"$PORT_DIR/src"
  -O1 -g -fno-omit-frame-pointer
  -Wall -Wextra -Werror -pedantic
  -fsanitize=address,undefined
)
for compiler in gcc clang; do
  test_binary="$WORK_ROOT/test-contract-$compiler"
  "$compiler" "${SANITIZER_FLAGS[@]}" \
    "$PORT_DIR/src/contract.c" "$PORT_DIR/tests/test_contract_tokens.c" \
    -o "$test_binary"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$test_binary"

  prefs_binary="$WORK_ROOT/test-playerprefs-$compiler"
  "$compiler" "${SANITIZER_FLAGS[@]}" \
    "$PORT_DIR/src/playerprefs_fix.c" \
    "$PORT_DIR/tests/test_playerprefs_overloads.c" \
    -o "$prefs_binary"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$prefs_binary"
done

gcc -std=c11 -D_POSIX_C_SOURCE=200809L -I"$PORT_DIR/src" \
  -Wall -Wextra -Werror -pedantic -fanalyzer -fsyntax-only \
  "$PORT_DIR/src/contract.c" "$PORT_DIR/tests/test_contract_tokens.c" \
  >"$WORK_ROOT/gcc-analyzer.txt" 2>&1
clang -std=c11 -D_POSIX_C_SOURCE=200809L -I"$PORT_DIR/src" \
  -Wall -Wextra -Werror -pedantic --analyze \
  -Xanalyzer -analyzer-output=text \
  "$PORT_DIR/src/contract.c" "$PORT_DIR/tests/test_contract_tokens.c" \
  >"$WORK_ROOT/clang-analyzer.txt" 2>&1
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -I"$PORT_DIR/src" \
  -Wall -Wextra -Werror -pedantic -fanalyzer -fsyntax-only \
  "$PORT_DIR/src/playerprefs_fix.c" \
  "$PORT_DIR/tests/test_playerprefs_overloads.c" \
  >"$WORK_ROOT/gcc-playerprefs-analyzer.txt" 2>&1
clang -std=c11 -D_POSIX_C_SOURCE=200809L -I"$PORT_DIR/src" \
  -Wall -Wextra -Werror -pedantic --analyze \
  -Xanalyzer -analyzer-output=text \
  "$PORT_DIR/src/playerprefs_fix.c" \
  "$PORT_DIR/tests/test_playerprefs_overloads.c" \
  >"$WORK_ROOT/clang-playerprefs-analyzer.txt" 2>&1

contract_args=()
if [[ ${BC_PREFREEZE:-0} == 1 ]]; then
  contract_args+=(--pre-freeze)
fi
python3 -B "$PORT_DIR/tests/test_contract.py" "${contract_args[@]}"

SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786233600}
export SOURCE_DATE_EPOCH
first_rel="$REL_WORK/first/bombchicken-nextos"
second_rel="$REL_WORK/second/bombchicken-nextos"
BC_UNIVERSAL_OUTPUT=$first_rel "$PORT_DIR/build.sh"
BC_UNIVERSAL_OUTPUT=$second_rel "$PORT_DIR/build.sh"
first="$PORT_DIR/$first_rel"
second="$PORT_DIR/$second_rel"
cmp -- "$first" "$second" || fail "two clean public builds differ"
python3 -B "$PORT_DIR/tests/test_contract.py" "${contract_args[@]}" \
  --loader "$first"

if [[ -n ${BC_OWNER_APK:-} ]]; then
  [[ -f $BC_OWNER_APK && ! -L $BC_OWNER_APK ]] ||
    fail "BC_OWNER_APK must name one regular, non-symlink APK"
  owner_before=$(sha256sum -- "$BC_OWNER_APK" | awk '{print $1}')
  fixture="$WORK_ROOT/owner-fixture"
  mkdir -p -- "$fixture/gamedata"
  cp --reflink=auto -- "$BC_OWNER_APK" "$fixture/gamedata/owner.apk"
  cp -- "$PORT_DIR/extractor.json" "$fixture/extractor.json"
  python3 -B "$PORT_DIR/nxextract/nxextract.py" install \
    --recipe "$fixture/extractor.json" --game-dir "$fixture" \
    --input "$fixture/gamedata/owner.apk" --ui none --quiet
  python3 -B "$PORT_DIR/nxextract/nxextract.py" verify \
    --recipe "$fixture/extractor.json" --game-dir "$fixture"
  owner_after=$(sha256sum -- "$BC_OWNER_APK" | awk '{print $1}')
  [[ $owner_before == "$owner_after" ]] || fail "owner APK changed during fixture"
  printf 'owner-data exact extraction fixture passed (temporary copy only)\n'
fi

git -C "$REPO_ROOT" diff --check -- ports/bombchicken
printf 'bombchicken deterministic loader sha256=%s\n' \
  "$(sha256sum -- "$first" | awk '{print $1}')"
printf '%s\n' \
  'BOMBCHICKEN HOST GATE PASS' \
  'physical_device_evidence=0' \
  'device_calls=0 network_calls=0 session_calls=0 guest_execution=0' \
  'sdl_runtime_calls=0 egl_runtime_calls=0 gles_runtime_calls=0 game_processes=0'
