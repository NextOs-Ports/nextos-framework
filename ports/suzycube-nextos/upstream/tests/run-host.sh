#!/usr/bin/env bash
# Hermetic release gate. It audits/builds but never executes owner game code.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
# shellcheck source=../tools/framework-source.sh
source "$PORT_DIR/tools/framework-source.sh"
SC_FRAMEWORK_REPOSITORY=$(sc_resolve_framework_repository "$PORT_DIR")
export SC_FRAMEWORK_REPOSITORY
FRAMEWORK_ROOT="$SC_FRAMEWORK_REPOSITORY/framework"
cd "$PORT_DIR"

INPUT_EVDEV_TEST=$(mktemp "${TMPDIR:-/tmp}/suzycube-input-evdev-test.XXXXXX")
INPUT_POLICY_TEST=$(mktemp "${TMPDIR:-/tmp}/suzycube-input-policy-test.XXXXXX")
cleanup() {
  rm -f -- "$INPUT_EVDEV_TEST" "$INPUT_POLICY_TEST"
}
trap cleanup EXIT INT TERM

"${HOSTCC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc \
  tests/test_input_evdev.c src/input_evdev.c -o "$INPUT_EVDEV_TEST"
"$INPUT_EVDEV_TEST"

SDL_CFLAGS=$(pkg-config --cflags sdl2)
"${HOSTCC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc $SDL_CFLAGS \
  -I"$FRAMEWORK_ROOT/nxinput/include" \
  -I"$FRAMEWORK_ROOT/nxinput/src" \
  tests/test_input_policy.c \
  "$FRAMEWORK_ROOT/nxinput/src/nxinput_core.c" \
  -lm -o "$INPUT_POLICY_TEST"
"$INPUT_POLICY_TEST"

for script in build_universal.sh "Suzy Cube.sh" tools/*.sh \
              nxextract/run-extractor.sh nxextract/nxextract-runtime-env.sh \
              package/build-package.sh; do
  bash -n "$script"
done

python3 -B nxextract/nxextract.py recipe-check --recipe extractor.json
python3 -B -m py_compile nxextract/merge-suzycube-data.py \
  package/render-manifest.py tests/test_suzycube_contract.py
./build_universal.sh
NXABI_REPORT=$(mktemp "${TMPDIR:-/tmp}/suzycube-nxabi.XXXXXX")
python3 -B "$FRAMEWORK_ROOT/nxabi/nxabi.py" audit \
  build/suzycube-nextos nxextract/nxextract-ui nxsplash-nextos \
  --json "$NXABI_REPORT" --quiet
rm -f -- "$NXABI_REPORT"
python3 -B package/render-manifest.py --check
python3 -B tests/test_suzycube_contract.py
python3 -B "$FRAMEWORK_ROOT/nxrelease/nxrelease.py" validate \
  --manifest nxrelease.json

DRY_ADD=$(git -C "$PORT_DIR" add -n --all .)
if grep -E \
  "add '(.build|build|gamedata|stage|verify|logs)/|add '(suzycube|suzycube-nextos)'|\.(apk|apkm|apks|xapk|obb|so|zip|raw)'" \
  <<< "$DRY_ADD"; then
  printf '%s\n' 'Git dry-run would stage built or owner data' >&2
  exit 1
fi

git -C "$PORT_DIR" diff --check
printf '%s\n' \
  'SUZY CUBE HOST GATE: PASS' \
  'physical_device_evidence=0 baseline_physical_release=1.1.0 proprietary_payload_packaged=0 guest_execution=0'
