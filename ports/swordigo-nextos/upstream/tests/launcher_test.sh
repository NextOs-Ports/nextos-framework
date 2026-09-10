#!/usr/bin/env bash
# Host-only contract for the current single self-contained launcher.
# No game, no SDL, no device.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
GENERATOR=${NXBOOTSTRAP_GENERATOR:-$ROOT/../nextos_ports_android/framework/nxbootstrap/tools/generate-port.py}

fail() { printf 'launcher test: %s\n' "$*" >&2; exit 1; }

POLICY_TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/swordigo-policy-test.XXXXXX")
cleanup() {
  case $POLICY_TEST_DIR in
    "${TMPDIR:-/tmp}"/swordigo-policy-test.*)
      rm -rf -- "$POLICY_TEST_DIR"
      ;;
    *) fail "unsafe provider-policy test directory: $POLICY_TEST_DIR" ;;
  esac
}
trap cleanup EXIT INT TERM
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/test_gl_provider_policy.c" \
  "$ROOT/src/gl_provider_policy.c" \
  -o "$POLICY_TEST_DIR/test-gl-provider-policy"
"$POLICY_TEST_DIR/test-gl-provider-policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/test_crash_diag.c" \
  "$ROOT/src/crash_diag.c" \
  -o "$POLICY_TEST_DIR/test-crash-diag"
"$POLICY_TEST_DIR/test-crash-diag"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/test_contract.c" \
  "$ROOT/src/contract.c" \
  -o "$POLICY_TEST_DIR/test-contract"
"$POLICY_TEST_DIR/test-contract"

# The checked-in launcher is generated output, never a hand-maintained fork.
[[ -f $GENERATOR && ! -L $GENERATOR ]] ||
  fail "canonical nxbootstrap generator is missing or linked: $GENERATOR"
GENERATOR_ROOT=$(CDPATH= cd -- "$(dirname -- "$GENERATOR")/.." && pwd -P)
NXBOOTSTRAP_VERSION=$(cat "$GENERATOR_ROOT/VERSION")
[ -n "$NXBOOTSTRAP_VERSION" ] || fail 'canonical nxbootstrap version is empty'
mkdir -p "$POLICY_TEST_DIR/generated"
python3 -B "$GENERATOR" "$ROOT/swordigo/nxport.json" \
  --output "$POLICY_TEST_DIR/generated" >/dev/null
cmp -s "$ROOT/Swordigo.sh" "$POLICY_TEST_DIR/generated/Swordigo.sh" ||
  fail "launcher diverges from canonical nxbootstrap $NXBOOTSTRAP_VERSION output"
cmp -s "$ROOT/swordigo/nxport.json" \
  "$POLICY_TEST_DIR/generated/swordigo/nxport.json" ||
  fail 'nxport.json is not in canonical generated form'

LAUNCHER=$ROOT/Swordigo.sh
bash -n "$LAUNCHER" || fail 'entry script is not valid Bash'
[[ $(head -n 1 "$LAUNCHER") == '#!/bin/bash' ]] ||
  fail 'launcher does not use the portable /bin/bash interpreter'
grep -Fq '# PORTMASTER: swordigo, Swordigo.sh' "$LAUNCHER" ||
  fail 'launcher lacks its PORTMASTER identity'
grep -Fq "nxbootstrap $NXBOOTSTRAP_VERSION" "$LAUNCHER" ||
  fail 'launcher does not record its generator version'

# Golden-port guarantees of the current generated shape.
for needle in \
  'source "$controlfolder/control.txt"' \
  'get_controls' \
  'GAMEDIR="/$directory/ports/swordigo"' \
  'flock -n 9' \
  'command ls -Lldn /proc/self/fd/9' \
  '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9' \
  'NXBOOTSTRAP_CHILD_STARTTIME' \
  'nxbootstrap_child_alive' \
  'nxbootstrap_finish' \
  '9>&- &' \
  'wait "$game_pid"' \
  "printf '\\033c'" \
  'pm_platform_helper' \
  'NXBOOTSTRAP_DIALOG_PIPE=${PM_PIPE:-}' \
  'PortMasterDialogExit' \
  'PM_PIPE remained after close request' \
  'pm_finish' \
  'swordigo-nextos'
do
  grep -Fq "$needle" "$LAUNCHER" || fail "launcher lacks canonical line: $needle"
done
if grep -Eq '(^|[;&|[:space:]])stat[[:space:]]+-' "$LAUNCHER"; then
  fail 'launcher still requires the external stat command'
fi
grep -Fq 'adapter.gl-provider-reexec-preload' "$LAUNCHER" ||
  fail 'launcher lost the post-context provider repair contract'
grep -Fq 'adapter.gl-provider-coherent-sdl-reexec' "$LAUNCHER" ||
  fail 'launcher lost the coherent SDL EGL/GLES provider repair contract'
grep -Fq 'adapter.gl-provider-probe-init-reexec' "$LAUNCHER" ||
  fail 'launcher lost the pre-context provider repair contract'
if ! python3 - "$LAUNCHER" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
portmaster = text.index('"$controlfolder/libs.aarch64"')
local = text.index('/usr/local/lib/aarch64-linux-gnu', portmaster)
system = text.index('/usr/lib/aarch64-linux-gnu', local)
game = text.index('$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}', system)
assert portmaster < local < system < game
PY
then
  fail 'launcher lost firmware-local library precedence'
fi
grep -Eq '@[A-Z0-9_]+@' "$LAUNCHER" && fail 'launcher has unresolved tokens' || true

# Retired layers must stay retired.
if grep -q 'run[.]sh' "$LAUNCHER"; then
  fail 'obsolete second-stage run.sh is still referenced'
fi
[ ! -e "$ROOT/run.sh" ] || fail 'obsolete run.sh still exists'
for retired in swordigo/nxbootstrap.sh swordigo/nxbootstrap-0.5.1.sh \
               swordigo/nxdeployment.json; do
  [ ! -e "$ROOT/$retired" ] || fail "retired artifact still present: $retired"
done
if grep -qE '^[^#]*export[[:space:]]+SDL_(VIDEODRIVER|AUDIODRIVER)=' \
    "$LAUNCHER"; then
  fail 'launcher forces an SDL video or audio driver'
fi

LOADER=$ROOT/swordigo-nextos
[ -f "$LOADER" ] && [ ! -L "$LOADER" ] && [ -x "$LOADER" ] ||
  fail 'public loader is missing, linked or not executable'
LC_ALL=C readelf -hW "$LOADER" | grep -q 'Machine:.*AArch64' ||
  fail 'public loader is not AArch64'
if LC_ALL=C readelf -dW "$LOADER" | grep -Eq '(RPATH|RUNPATH)'; then
  fail 'public loader embeds RPATH/RUNPATH'
fi
loader_glibc=$(LC_ALL=C readelf --version-info "$LOADER" |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
[ -n "$loader_glibc" ] || fail 'public loader has no readable glibc ABI'
loader_version=${loader_glibc#GLIBC_}
loader_major=${loader_version%%.*}
loader_minor=${loader_version#*.}; loader_minor=${loader_minor%%.*}
if [ "$loader_major" -gt 2 ] ||
   { [ "$loader_major" -eq 2 ] && [ "$loader_minor" -gt 30 ]; }; then
  fail "public loader requires $loader_glibc; ceiling is GLIBC_2.30"
fi

# The GL-provider repair (crossed-SONAME black screen) must ship in the loader.
# grep without -q: -q quits early and SIGPIPEs strings under pipefail.
LC_ALL=C strings "$LOADER" | grep 'SWORDIGO_GLFIX' >/dev/null ||
  fail 'loader lost the glfix provider repair'
LC_ALL=C strings "$LOADER" | grep 'SDL_VIDEO_GL_DRIVER' >/dev/null ||
  fail 'loader lost the coherent SDL provider pair'
grep -q 'glfix_maybe_reexec' "$ROOT/src/main.c" ||
  fail 'loader source no longer calls the glfix repair'
grep -q 'swordigo_contract_quirk_enabled' "$ROOT/src/glfix.c" ||
  fail 'provider repair is no longer controlled by the manifest contract'

grep -qE '^\s*drivers\s*=\s*pipewire,pulse,alsa\s*$' "$ROOT/alsoft.conf" ||
  fail 'alsoft.conf does not preserve the approved backend order'
grep -q 'configure_runtime_environment' "$ROOT/src/main.c" ||
  fail 'adapter-owned runtime environment is not initialized in the loader'
grep -q 'SWORDIGO_DEBUG_CONTROL' "$ROOT/src/main.c" ||
  fail 'developer control channel is not opt-in'
grep -q 'SWORDIGO_GLFINISH' "$ROOT/src/main.c" ||
  fail 'KMSDRM present synchronization has no explicit policy override'
if grep -q 'fopen(LOG_NAME' "$ROOT/src/util.c"; then
  fail 'loader still competes with the launcher for the durable log'
fi

python3 -B "$ROOT/nxextract.py" recipe-check --recipe "$ROOT/extractor.json" \
  >/dev/null || fail 'NXExtract rejected the Swordigo recipe'
grep -q '^1[.]2[.]10$' "$ROOT/nxextract-version.txt" ||
  fail 'NXExtract version is not 1.2.10'

printf 'swordigo single-launcher tests passed\n'
