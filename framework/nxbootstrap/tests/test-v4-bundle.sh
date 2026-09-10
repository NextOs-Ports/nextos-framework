#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V4-REPACK-01 host gate. It generates a REAL port, proves the visible
# nxbundle-v1 seed exists and is deterministic, then reinstalls the port the
# way a personal rezip does it -- every dotfile and dotdir dropped -- and
# boots the REAL generated launcher. The runtime cache must be rebuilt from
# the seed alone, with `commit` written last and the closure authenticated by
# the launcher's baked generation id. Negatives: absent, truncated, tampered
# and foreign-generation seeds, a launcher that does not belong to the seed,
# and a seed carrying a `commit` member.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

command -v sha256sum >/dev/null 2>&1 || {
  printf 'v4 bundle test refused: sha256sum is required on the host\n' >&2
  exit 77
}

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-v4-bundle.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-v4-bundle.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'v4 bundle test failed: %s\n' "$*" >&2
  if [[ -n ${LOG-} && -f $LOG ]]; then
    printf '%s\n' '--- launcher log ---' >&2
    sed -n '1,260p' "$LOG" >&2 || true
    printf '%s\n' '--- previous launcher log ---' >&2
    sed -n '1,180p' "${LOG%.txt}.prev.txt" >&2 || true
  fi
  exit 1
}

# Force the fixture control root in this private mount namespace.
for masked in /opt /boot /storage/roms/ports /roms/ports; do
  if [[ -d $masked ]]; then
    mount -t tmpfs -o size=64k,mode=0755 tmpfs "$masked" 2>/dev/null ||
      fail "cannot mask preferred PortMaster root $masked"
  fi
done

REAL_ROOT=$TEST_ROOT/real
PORTS_DIR=$REAL_ROOT/roms/ports
PM_DIR=$TEST_ROOT/xdg/PortMaster
MARKERS=$TEST_ROOT/markers
RUNTIME_DIR=$TEST_ROOT/runtime
mkdir -p "$PORTS_DIR" "$PM_DIR" "$MARKERS"
mkdir -m 0700 "$RUNTIME_DIR"
cat > "$PM_DIR/control.txt" <<CONTROL
directory="${TEST_ROOT#/}/real/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=v4-bundle-fixture
sdl_controllerconfig=""
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

PORT_ID=v4-bundle-port
LAUNCHER_NAME='V4 Bundle.sh'
PORT_DIR=$PORTS_DIR/$PORT_ID
LAUNCHER=$PORTS_DIR/$LAUNCHER_NAME
LOG=$PORT_DIR/log.txt
STATE=$PORT_DIR/.nxruntime/state.json
RUN_MARKER=$MARKERS/runtime-labels

write_runtime() {
  local root=$1 label=$2
  mkdir -p "$root/lib" "$root/nxextract/helpers" "$root/nxextract/specs"
  cat > "$root/v4-bundle-loader" <<STUB
#!/bin/bash
[ "\${NXV2_HOOK_LABEL:-}" = "$label" ] || exit 12
# The fixture has to stand on its own: installed by PortMaster on a real
# device there is no test shell to hand it a marker path, and under \`set -u\`
# an unset one killed the stub before it did anything. The gate still passes
# its own path; without one the fixture keeps its record beside the port.
nxv2_run_marker=\${NXV2_RUN_MARKER:-\$NXCOMPAT_GAME_DIR_PHYSICAL/.nxv2-run-marker}
printf '%s\n' '$label' >> "\$nxv2_run_marker"
health_tmp="\$NXBOOTSTRAP_HEALTH_FILE.tmp.\$\$"
(umask 077; set -C; printf '%s\n' \
  "{\"schema\":\"\$NXBOOTSTRAP_HEALTH_SCHEMA\",\"schema_version\":\$NXBOOTSTRAP_HEALTH_SCHEMA_VERSION,\"run_id\":\"\$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"\$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"\$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" \
  > "\$health_tmp") || exit 13
mv -f "\$health_tmp" "\$NXBOOTSTRAP_HEALTH_FILE" || exit 13
if [ "\${NXV2_BREAK_STATE_WRITE:-0}" = 1 ]; then
  rm -f -- "\$NXCOMPAT_GAME_DIR_PHYSICAL/.nxruntime/state.json" || exit 14
  mkdir "\$NXCOMPAT_GAME_DIR_PHYSICAL/.nxruntime/state.json" || exit 14
fi
exit 0
STUB
  chmod 0755 "$root/v4-bundle-loader"
  printf 'private library %s\n' "$label" > "$root/lib/libprivate.so"
  chmod 0644 "$root/lib/libprivate.so"
  cat > "$root/port-env.sh" <<HOOK
export NXV2_HOOK_LABEL='$label'
HOOK
  chmod 0644 "$root/port-env.sh"
  printf '{"fixture":"v4-bundle-nxextract","label":"%s"}\n' \
    "$label" > "$root/extractor.json"
  chmod 0644 "$root/extractor.json"
  printf '# NXExtract engine fixture LABEL=%s\n' "$label" \
    > "$root/nxextract/nxextract.py"
  chmod 0644 "$root/nxextract/nxextract.py"
  cat > "$root/nxextract/nxextract-runtime-env.sh" <<ENV
export NXV2_NXENV_LABEL='$label'
exec "\$@"
ENV
  chmod 0644 "$root/nxextract/nxextract-runtime-env.sh"
  cat > "$root/nxextract/helpers/prepare.py" <<PY
import json
import pathlib
import sys

spec = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
if spec != {"fixture": "v4-bundle-nxextract", "label": "$label"}:
    raise SystemExit(41)
engine = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
if "LABEL=$label" not in engine:
    raise SystemExit(42)
PY
  chmod 0644 "$root/nxextract/helpers/prepare.py"
  printf '{"fixture":"v4-bundle-nxextract","label":"%s"}\n' \
    "$label" > "$root/nxextract/specs/prepare.json"
  chmod 0644 "$root/nxextract/specs/prepare.json"
  cat > "$root/nxextract/nxextract-ui" <<UI
#!/bin/sh
# LABEL=$label
exit 0
UI
  chmod 0755 "$root/nxextract/nxextract-ui"
  cat > "$root/nxextract/run-extractor.sh" <<RUNNER
#!/usr/bin/env bash
set -eu
[ "\${NXV2_NXENV_LABEL:-}" = "$label" ] || exit 43
grep -Fq '"label":"$label"' "\$NXEXTRACT_GAME_DIR/extractor.json" || exit 44
grep -Fq 'LABEL=$label' "\$NXEXTRACT_GAME_DIR/nxextract/nxextract-ui" || exit 45
python3 -B "\$NXEXTRACT_GAME_DIR/nxextract/helpers/prepare.py" \
  "\$NXEXTRACT_GAME_DIR/nxextract/specs/prepare.json" \
  "\$NXEXTRACT_GAME_DIR/nxextract/nxextract.py" || exit 46
nxv2_nxextract_marker=\${NXV2_NXEXTRACT_MARKER:-\$NXEXTRACT_GAME_DIR/.nxv2-nxextract-marker}
printf '%s\n' '$label' >> "\$nxv2_nxextract_marker"
python3 -B - "\$NXEXTRACT_GAME_DIR/nxextract-result.json" <<'PYRESULT'
import json
import sys

result = {
    "schema": "org.nextos.nxextract.terminal-result",
    "schema_version": 1,
    "nxextract_version": "1.2.21",
    "outcome": "success",
    "code": "NXE0000",
    "final_phase": {"index": 8, "id": "ready", "label": "Ready"},
    "recipe": {"id": "v4-bundle-nxextract", "version": "fixture-1",
               "digest": "0" * 64},
    "package_id": "org.nextos.v4_bundle_nxextract",
    "abi": "arm64-v8a",
    "container": {"kind": "existing", "identity": "1" * 64},
    "validated": {"items": 1, "bytes": 1, "critical_payloads": []},
    "logs": {"summary": "nxextract.log", "detail": "nxextract-detail.log"},
    "duration_ms": 1,
    "completed_unix": 1,
    "ui": {"mode": "visible", "renderer": "sdl", "fallback_reason": None},
    "error": None,
}
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(result, stream, sort_keys=True, separators=(",", ":"))
    stream.write("\n")
PYRESULT
RUNNER
  chmod 0644 "$root/nxextract/run-extractor.sh"
}

write_manifest() {
  local target=$1 runtime_root=$2 title=$3 executable_hash library_hash hook_hash
  local recipe_hash engine_hash runner_hash env_hash ui_hash helper_hash spec_hash
  executable_hash=$(sha256sum "$runtime_root/v4-bundle-loader")
  executable_hash=${executable_hash%% *}
  library_hash=$(sha256sum "$runtime_root/lib/libprivate.so")
  library_hash=${library_hash%% *}
  hook_hash=$(sha256sum "$runtime_root/port-env.sh")
  hook_hash=${hook_hash%% *}
  recipe_hash=$(sha256sum "$runtime_root/extractor.json")
  recipe_hash=${recipe_hash%% *}
  engine_hash=$(sha256sum "$runtime_root/nxextract/nxextract.py")
  engine_hash=${engine_hash%% *}
  runner_hash=$(sha256sum "$runtime_root/nxextract/run-extractor.sh")
  runner_hash=${runner_hash%% *}
  env_hash=$(sha256sum "$runtime_root/nxextract/nxextract-runtime-env.sh")
  env_hash=${env_hash%% *}
  ui_hash=$(sha256sum "$runtime_root/nxextract/nxextract-ui")
  ui_hash=${ui_hash%% *}
  helper_hash=$(sha256sum "$runtime_root/nxextract/helpers/prepare.py")
  helper_hash=${helper_hash%% *}
  spec_hash=$(sha256sum "$runtime_root/nxextract/specs/prepare.json")
  spec_hash=${spec_hash%% *}
  cat > "$target" <<JSON
{
  "schema_version": 3,
  "id": "$PORT_ID",
  "title": "$title",
  "launcher_name": "$LAUNCHER_NAME",
  "architecture": "aarch64",
  "executable": "v4-bundle-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "yes", "version": "1.2.21"},
  "required_files": ["v4-bundle-loader", "lib/libprivate.so", "port-env.sh"],
  "private_library_paths": ["lib"],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log",
  "generation_runtime": [
    {"role":"executable","path":"v4-bundle-loader","mode":"0755","sha256":"$executable_hash"},
    {"role":"private-library","path":"lib/libprivate.so","mode":"0644","sha256":"$library_hash"},
    {"role":"runtime-hook","path":"port-env.sh","mode":"0644","sha256":"$hook_hash"},
    {"role":"nxextract-recipe","path":"extractor.json","mode":"0644","sha256":"$recipe_hash"},
    {"role":"nxextract-engine","path":"nxextract/nxextract.py","mode":"0644","sha256":"$engine_hash"},
    {"role":"nxextract-runner","path":"nxextract/run-extractor.sh","mode":"0644","sha256":"$runner_hash"},
    {"role":"nxextract-runtime-env","path":"nxextract/nxextract-runtime-env.sh","mode":"0644","sha256":"$env_hash"},
    {"role":"nxextract-ui","path":"nxextract/nxextract-ui","mode":"0755","sha256":"$ui_hash"},
    {"role":"nxextract-helper","path":"nxextract/helpers/prepare.py","mode":"0644","sha256":"$helper_hash"},
    {"role":"nxextract-spec","path":"nxextract/specs/prepare.json","mode":"0644","sha256":"$spec_hash"}
  ]
}
JSON
}

RUNTIME_A=$TEST_ROOT/runtime-a
RUNTIME_B=$TEST_ROOT/runtime-b
GEN_A=$TEST_ROOT/generated-a
GEN_B=$TEST_ROOT/generated-b
write_runtime "$RUNTIME_A" A
write_runtime "$RUNTIME_B" B
write_manifest "$TEST_ROOT/nxport-a.json" "$RUNTIME_A" 'V4 Bundle A'
write_manifest "$TEST_ROOT/nxport-b.json" "$RUNTIME_B" 'V4 Bundle B'

# The production NXSplash is an AArch64 ELF. The host behavioral gate replaces
# only the generator's artifact provider with the existing byte-pinned host
# splash stub, so NXSplash remains a real hashed generation-v2 component.
generate_fixture() {
  python3 -B - "$PROJECT_ROOT/tools/generate-port.py" "$TEST_DIR/splash-stub.sh" \
    "$1" "$2" "$3" <<'PY'
import hashlib
import importlib.util
import sys
from pathlib import Path

generator_path, splash_path, manifest_path, output_path, runtime_root = sys.argv[1:]
spec = importlib.util.spec_from_file_location("v4_bundle_fixture", generator_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
splash = Path(splash_path).resolve()
digest = hashlib.sha256(splash.read_bytes()).hexdigest()
module.nxsplash_artifact = lambda _architecture: (splash, digest)
module.generate(Path(manifest_path), Path(output_path), False, Path(runtime_root))
PY
}
generate_fixture "$TEST_ROOT/nxport-a.json" "$GEN_A" "$RUNTIME_A" ||
  fail 'generator refused generation A'
generate_fixture "$TEST_ROOT/nxport-b.json" "$GEN_B" "$RUNTIME_B" ||
  fail 'generator refused generation B'

read_generation_id() {
  local commits=("$1/$PORT_ID/.nxruntime/generations"/*/commit)
  [[ ${#commits[@]} == 1 && -f ${commits[0]} ]] ||
    fail "$1 does not contain exactly one committed generation"
  cat "${commits[0]}"
}
GA_ID=$(read_generation_id "$GEN_A")
GB_ID=$(read_generation_id "$GEN_B")
[[ $GA_ID =~ ^[0-9a-f]{64}$ && $GB_ID =~ ^[0-9a-f]{64}$ &&
   $GA_ID != "$GB_ID" ]] || fail 'generation-v2 identities are not distinct SHA-256 values'

for generated in "$GEN_A" "$GEN_B"; do
  generation_id=$(read_generation_id "$generated")
  generation_root=$generated/$PORT_ID/.nxruntime/generations/$generation_id
  [[ $(<"$generation_root/format") == nxruntime-generation-v2 ]] ||
    fail 'generation is not self-describing v2'
  [[ -f $generation_root/identity.json &&
     -f $generation_root/identity-runtime.v2 &&
     $(sha256sum "$generation_root/identity.json" | cut -d' ' -f1) == "$generation_id" ]] ||
    fail 'stored generation identity is not bound to its directory id'
  grep -Fq $'runtime-hook\t0644\t' "$generation_root/components.v2" ||
    fail '0644 sourced runtime hook was not pinned'
  for nxrole in nxextract-recipe nxextract-engine nxextract-runner \
    nxextract-runtime-env nxextract-ui nxextract-helper nxextract-spec; do
    grep -Fq "$nxrole"$'\t' "$generation_root/components.v2" ||
      fail "$nxrole is absent from generation v2"
  done
  grep -Fq $'nxsplash\t0755\t' "$generation_root/components.v2" ||
    fail 'canonical NXSplash is absent from generation v2'
  [[ -f $generation_root/files/runtime/v4-bundle-loader &&
     -f $generation_root/files/runtime/lib/libprivate.so &&
     -f $generation_root/files/runtime/port-env.sh &&
     -f $generation_root/files/runtime/extractor.json &&
     -f $generation_root/files/runtime/nxextract/nxextract.py &&
     -f $generation_root/files/runtime/nxextract/run-extractor.sh &&
     -f $generation_root/files/runtime/nxextract/nxextract-runtime-env.sh &&
     -f $generation_root/files/runtime/nxextract/nxextract-ui &&
     -f $generation_root/files/runtime/nxextract/helpers/prepare.py &&
     -f $generation_root/files/runtime/nxextract/specs/prepare.json &&
     -f $generation_root/files/runtime/nxsplash-nextos ]] ||
    fail 'runtime closure was not copied below files/runtime'
  ! find "$generation_root/files/runtime" -type f \( \
    -name NEXTOSCONTROLLERS.gptk -o -name NEXTOSSETTINGS.txt \) | grep -q . ||
    fail 'owner files entered the immutable generation'
done


run_launcher() {
  local path_prefix=${1:-$PATH} status=0
  : > "$MARKERS/pm-finish"
  env -i PATH="$path_prefix" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    NXV2_RUN_MARKER="$RUN_MARKER" \
    NXV2_NXEXTRACT_MARKER="$MARKERS/nxextract-labels" \
    NXV2_BREAK_STATE_WRITE="${NXV2_BREAK_STATE_WRITE:-0}" \
    NXV2_FAIL_STATE_TARGET="${NXV2_FAIL_STATE_TARGET:-}" \
    NXV2_FAIL_STAGE_SOURCE="${NXV2_FAIL_STAGE_SOURCE:-}" \
    NX_TEST_CHMODLESS_PREFIX="${NX_TEST_CHMODLESS_PREFIX:-}" \
    bash "$LAUNCHER" </dev/null >/dev/null 2>&1 || status=$?
  return "$status"
}

run_launcher_without_fixture_markers() {
  local path_prefix=${1:-$PATH} status=0
  : > "$MARKERS/pm-finish"
  env -i PATH="$path_prefix" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    NXV2_BREAK_STATE_WRITE="${NXV2_BREAK_STATE_WRITE:-0}" \
    NXV2_FAIL_STATE_TARGET="${NXV2_FAIL_STATE_TARGET:-}" \
    NXV2_FAIL_STAGE_SOURCE="${NXV2_FAIL_STAGE_SOURCE:-}" \
    NX_TEST_CHMODLESS_PREFIX="${NX_TEST_CHMODLESS_PREFIX:-}" \
    bash "$LAUNCHER" </dev/null >/dev/null 2>&1 || status=$?
  return "$status"
}


# ---------------------------------------------------------------- V4 seed shape
BUNDLE_A=$GEN_A/$PORT_ID/nxruntime-$GA_ID.nxb
BUNDLE_B=$GEN_B/$PORT_ID/nxruntime-$GB_ID.nxb
[[ -f $BUNDLE_A && ! -L $BUNDLE_A ]] ||
  fail 'the generated port does not carry a visible runtime seed'
[[ -f $BUNDLE_B && ! -L $BUNDLE_B ]] ||
  fail 'generation B does not carry a visible runtime seed'
case "$(ls -ld -- "$BUNDLE_A")" in
  -rw-r--r--*) ;;
  *) fail 'the runtime seed is not a regular 0644 file' ;;
esac
[[ $(head -n 1 "$BUNDLE_A") == NXBUNDLE1 ]] ||
  fail 'the runtime seed is not nxbundle-v1'
grep -Fqx "generation $GA_ID" <(head -n 4 "$BUNDLE_A") ||
  fail 'the runtime seed does not declare its generation'
! grep -Fq $'\tcommit' "$BUNDLE_A" ||
  fail 'the runtime seed transports a commit marker'
# The seed is content addressed: regenerating from identical inputs must
# reproduce identical bytes, with no timestamp or ordering leaking in.
GEN_A2=$TEST_ROOT/generated-a2
generate_fixture "$TEST_ROOT/nxport-a.json" "$GEN_A2" "$RUNTIME_A" ||
  fail 'generator refused the determinism rerun'
cmp -s "$BUNDLE_A" "$GEN_A2/$PORT_ID/nxruntime-$GA_ID.nxb" ||
  fail 'the runtime seed is not deterministic'

# ------------------------------------------------------- personal-rezip install
# Exactly what a graphical archiver produces: the two PortMaster root items,
# every dotfile/dotdir dropped, modes normalized to 0777, owner data added
# under gamedata/ and nothing else.
install_personal_rezip() {
  local generated=$1
  rm -rf -- "$PORT_DIR" "$LAUNCHER"
  mkdir -p "$PORT_DIR"
  ( cd "$generated/$PORT_ID" && find . -mindepth 1 \
      \( -name '.*' -prune \) -o -print0 ) |
    ( cd "$generated/$PORT_ID" && cpio -0 -pdm --quiet "$PORT_DIR" ) ||
    fail 'personal rezip fixture could not copy the port tree'
  cp -a "$generated/$LAUNCHER_NAME" "$LAUNCHER"
  chmod -R 0777 "$PORT_DIR"
  mkdir -p "$PORT_DIR/gamedata"
  printf 'owner payload\n' > "$PORT_DIR/gamedata/owner.bin"
  chmod 0777 "$PORT_DIR/gamedata/owner.bin"
}

install_personal_rezip "$GEN_A"
[[ ! -e $PORT_DIR/.nxruntime ]] ||
  fail 'the personal rezip fixture kept a dotdir'
OWNER_BEFORE=$(sha256sum "$PORT_DIR/gamedata/owner.bin")
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "personal rezip install exited $status"
grep -Fq "UPDATE NXU0012: runtime cache rebuilt from seed nxruntime-$GA_ID.nxb" "$LOG" ||
  fail 'the runtime cache was not rebuilt from the seed'
grep -Fq 'STORAGE: bundle=' "$LOG" ||
  fail 'the storage preflight receipt is missing'
[[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
  fail 'the rebuilt generation has no commit marker'
[[ $(<"$PORT_DIR/.nxruntime/generations/$GA_ID/commit") == "$GA_ID" ]] ||
  fail 'the rebuilt commit marker does not name its generation'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'the generation rebuilt from the seed was not promoted'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] ||
  fail 'the rebuilt closure did not run the real executable/hook'
[[ $(tail -n 1 "$MARKERS/nxextract-labels") == A ]] ||
  fail 'the rebuilt closure did not run the real NXExtract members'
[[ $(sha256sum "$PORT_DIR/gamedata/owner.bin") == "$OWNER_BEFORE" ]] ||
  fail 'owner data was touched by the rebuild'
[[ -z $(ls -d "$PORT_DIR/.nxruntime"/staging.* 2>/dev/null) ]] ||
  fail 'a staging directory survived a successful rebuild'

# Deleting the whole local cache must be fully recoverable from the seed only.
rm -rf -- "$PORT_DIR/.nxruntime"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "cache removal was not recoverable (exit $status)"
[[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
  fail 'the cache was not reconstructed after deletion'

# A PortMaster install has no surrounding test shell and therefore does not
# provide the fixture-only marker variables. Prove that the generated fixture
# reaches NXExtract, NXSplash and the runtime with those variables genuinely
# absent, preserving its evidence beside the installed port instead.
install_personal_rezip "$GEN_A"
rm -f -- "$PORT_DIR/.nxv2-nxextract-marker" "$PORT_DIR/.nxv2-run-marker"
status=0; run_launcher_without_fixture_markers || status=$?
[[ $status == 0 ]] ||
  fail "the self-sufficient PortMaster fixture exited $status"
[[ -f $PORT_DIR/.nxv2-nxextract-marker &&
   ! -L $PORT_DIR/.nxv2-nxextract-marker &&
   $(<"$PORT_DIR/.nxv2-nxextract-marker") == A ]] ||
  fail 'NXExtract still depends on the test-shell marker variable'
[[ -f $PORT_DIR/.nxv2-run-marker &&
   ! -L $PORT_DIR/.nxv2-run-marker &&
   $(<"$PORT_DIR/.nxv2-run-marker") == A ]] ||
  fail 'the runtime still depends on the test-shell marker variable'
grep -F 'NXEXTRACT_RESULT ' "$LOG" | grep -Fq '"outcome":"success"' ||
  fail 'the marker-free fixture produced no valid NXExtract terminal result'
grep -Fq 'PHASE nxextract OK' "$LOG" ||
  fail 'the marker-free fixture did not complete NXExtract'
grep -Fq 'PHASE nxsplash OK' "$LOG" ||
  fail 'the marker-free fixture did not complete NXSplash'
grep -Fq 'PHASE runtime EXIT status=0' "$LOG" ||
  fail 'the marker-free fixture did not complete the runtime'
[[ $(wc -l < "$MARKERS/pm-finish") -eq 1 ]] ||
  fail 'the marker-free fixture did not finish PortMaster exactly once'

# ------------------------------------------------------------------- negatives
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused() {
  local context=$1 evidence=$2 status=0
  run_launcher || status=$?
  [[ $status != 0 ]] || fail "$context launched"
  grep -Fq "$evidence" "$LOG" || fail "$context lacks its receipt: $evidence"
  [[ $(wc -l < "$RUN_MARKER") -eq $runtime_runs_before ]] ||
    fail "$context reached the game"
  [[ -z $(ls -d "$PORT_DIR/.nxruntime"/staging.* 2>/dev/null) ]] ||
    fail "$context left staging behind"
}

# absent seed
install_personal_rezip "$GEN_A"
rm -f -- "$PORT_DIR/nxruntime-$GA_ID.nxb"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'absent seed' \
  'UPDATE NXU0009: generation-v2 closure is absent; launch refused'

# truncated seed
install_personal_rezip "$GEN_A"
BUNDLE_SIZE=$(wc -c < "$PORT_DIR/nxruntime-$GA_ID.nxb")
head -c $((BUNDLE_SIZE - 64)) "$GEN_A/$PORT_ID/nxruntime-$GA_ID.nxb" \
  > "$PORT_DIR/nxruntime-$GA_ID.nxb"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'truncated seed' 'UPDATE NXU0012:'

# tampered payload byte: the header hash no longer matches
install_personal_rezip "$GEN_A"
python3 -B - "$PORT_DIR/nxruntime-$GA_ID.nxb" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r+b") as stream:
    data = bytearray(stream.read())
    end = data.index(b"\nEND\n") + 5
    data[end] ^= 0x01
    stream.seek(0)
    stream.write(data)
PY
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'tampered seed payload' \
  'UPDATE NXU0012: runtime seed member'

# a seed that belongs to another generation
install_personal_rezip "$GEN_A"
cp -a "$GEN_B/$PORT_ID/nxruntime-$GB_ID.nxb" \
  "$PORT_DIR/nxruntime-$GA_ID.nxb"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'foreign generation seed' \
  "UPDATE NXU0012: runtime seed declares generation $GB_ID"

# an installed launcher that is not the one the seed authenticates
install_personal_rezip "$GEN_A"
printf '\n# stray comment\n' >> "$LAUNCHER"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'launcher outside the seed closure' \
  'UPDATE NXU0012: installed launcher does not match the runtime seed closure'

# a rewritten header (member hash edited to match tampered bytes) still fails,
# because identity.json must hash to the generation id baked in the launcher
install_personal_rezip "$GEN_A"
python3 -B - "$PORT_DIR/nxruntime-$GA_ID.nxb" <<'PY'
import hashlib
import sys

path = sys.argv[1]
data = open(path, "rb").read()
end = data.index(b"\nEND\n") + 5
header, payload = data[:end], bytearray(data[end:])
lines = header.decode("ascii").split("\n")
records = [line for line in lines if line.startswith("M\t")]
target = next(line for line in records if line.endswith("\tidentity.json"))
_kind, mode, _digest, size, offset, name = target.split("\t")
size, offset = int(size), int(offset)
payload[offset] ^= 0x20
new_digest = hashlib.sha256(bytes(payload[offset:offset + size])).hexdigest()
replacement = "\t".join(["M", mode, new_digest, str(size), str(offset), name])
open(path, "wb").write(
    header.replace(target.encode("ascii"), replacement.encode("ascii"))
    + bytes(payload)
)
PY
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'consistently rewritten seed header' \
  'UPDATE NXU0012: materialized closure did not authenticate'

# ------------------------------------------------------ update over a personal ZIP
# The official B seed lands next to the owner's A tree; gamedata must survive.
install_personal_rezip "$GEN_A"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "A baseline for the update exited $status"
cp -a "$GEN_B/$PORT_ID/nxruntime-$GB_ID.nxb" "$PORT_DIR/"
cp -a "$GEN_B/$PORT_ID/nxport.json" "$PORT_DIR/nxport.json"
cp -a "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
rm -f -- "$PORT_DIR/nxruntime-$GA_ID.nxb"
OWNER_BEFORE=$(sha256sum "$PORT_DIR/gamedata/owner.bin")
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "update over a personal ZIP exited $status"
[[ -f $PORT_DIR/.nxruntime/generations/$GB_ID/commit ]] ||
  fail 'the update did not materialize generation B'
[[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
  fail 'the update destroyed the previous generation'
[[ $(tail -n 1 "$RUN_MARKER") == B ]] ||
  fail 'the update did not run the B closure'
[[ $(sha256sum "$PORT_DIR/gamedata/owner.bin") == "$OWNER_BEFORE" ]] ||
  fail 'the update touched owner data'

# ---------------------------------------------- real archiver rezip round trip
# The seed must survive every archiver freedom the contract lists: member order,
# timestamps, compression method, extra fields, creator system, DOS attributes
# and Unix modes that are absent, 0644 or normalized to 0777. Only the two
# PortMaster root items are ever repacked.
rezip_cycle() {
  local label=$1 tool=$2 work="$TEST_ROOT/rezip-$1" status=0
  rm -rf -- "$work"; mkdir -p "$work/src"
  ( cd "$GEN_A/$PORT_ID" && find . -mindepth 1 \( -name '.*' -prune \) -o -print0 ) |
    ( cd "$GEN_A/$PORT_ID" && cpio -0 -pdm --quiet "$work/src/$PORT_ID" ) ||
    fail "$label: could not stage the port tree"
  cp -a "$GEN_A/$LAUNCHER_NAME" "$work/src/$LAUNCHER_NAME"
  case $tool in
    infozip)
      ( cd "$work/src" && zip -q -r -X "$work/personal.zip" "$LAUNCHER_NAME" "$PORT_ID" ) ||
        fail "$label: Info-ZIP could not create the personal ZIP" ;;
    sevenzip)
      # The isolated suite runs behind a real RLIMIT_AS fence, and 7-Zip's
      # multi-threaded allocation does not fit inside it: it dies with
      # "Can't allocate required memory". Bound it to one thread. What this
      # variant tests is member order, compression method, timestamps and
      # extra fields -- never the archiver's appetite -- so a single thread
      # proves exactly the same contract.
      ( cd "$work/src" && 7z a -tzip -bso0 -bsp0 -mmt=off \
          -mm=Deflate "$work/personal.zip" \
          "$LAUNCHER_NAME" "$PORT_ID" >/dev/null ) ||
        fail "$label: 7-Zip could not create the personal ZIP" ;;
    stored-shuffled)
      python3 -B "$TEST_DIR/v4-bundle-dos-zip.py" "$work/src" "$work/personal.zip" \
        "$LAUNCHER_NAME" "$PORT_ID" ||
        fail "$label: the DOS-style personal ZIP could not be written" ;;
  esac
  rm -rf -- "$PORT_DIR" "$LAUNCHER"
  mkdir -p "$PORTS_DIR"
  unzip -qq -o "$work/personal.zip" -d "$PORTS_DIR" ||
    fail "$label: the personal ZIP could not be installed"
  [[ -f $LAUNCHER && -d $PORT_DIR ]] ||
    fail "$label: the PortMaster root layout did not survive"
  [[ ! -e $PORT_DIR/.nxruntime ]] ||
    fail "$label: the fixture did not drop the dotdir"
  chmod -R 0777 "$PORT_DIR"; chmod 0777 "$LAUNCHER"
  mkdir -p "$PORT_DIR/gamedata"
  printf 'owner payload\n' > "$PORT_DIR/gamedata/owner.bin"
  run_launcher || status=$?
  [[ $status == 0 ]] || fail "$label: the repacked port exited $status"
  [[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
    fail "$label: the runtime cache was not rebuilt"
  grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
    fail "$label: the repacked port did not promote its generation"
}
# PortMaster REWRITES line 2 of an installed launcher with the name of the zip
# it came from. Measured on the authorized device with a real
# `harbourmaster install`: exactly those bytes change, and a byte-for-byte
# anchor then refuses to launch the port -- the one install path users
# actually take. The identity is therefore taken over the canonical form.
install_personal_rezip "$GEN_A"
portmaster_marker="# PORTMASTER: some-other-name.zip, $LAUNCHER_NAME"
python3 - "$PORTS_DIR/$LAUNCHER_NAME" "$portmaster_marker" <<'PYEOF'
import sys
path, marker = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as handle:
    lines = handle.read().split("\n")
assert lines[1].startswith("# PORTMASTER: "), lines[1][:60]
lines[1] = marker
with open(path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines))
PYEOF
runtime_runs_before=$(wc -l < "$RUN_MARKER")
status=0
run_launcher || status=$?
[[ $status == 0 ]] ||
  fail "a PortMaster-rewritten marker line broke the launch ($status)"
grep -Fq "runtime cache rebuilt from seed" "$LOG" ||
  fail 'the rewritten marker stopped the cache rebuild'
[[ $(wc -l < "$RUN_MARKER") -gt $runtime_runs_before ]] ||
  fail 'the rewritten marker stopped the game from running'
# Only that one comment line is forgiven: a byte changed anywhere else in the
# installed launcher is still a refusal.
install_personal_rezip "$GEN_A"
printf '\n# smuggled\n' >> "$PORTS_DIR/$LAUNCHER_NAME"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'a launcher modified outside the installer-owned marker' \
  'installed launcher does not match the runtime seed closure'

rezip_cycle infozip infozip
rezip_cycle sevenzip sevenzip
rezip_cycle dos stored-shuffled

# --------------------------------------------------- structural seed negatives
seed_negative() {
  local label=$1 evidence=$2
  shift 2
  install_personal_rezip "$GEN_A"
  python3 -B "$TEST_DIR/v4-bundle-mutate.py" "$PORT_DIR/nxruntime-$GA_ID.nxb" "$@" ||
    fail "$label: mutation failed"
  runtime_runs_before=$(wc -l < "$RUN_MARKER")
  assert_refused "$label" "$evidence"
}
seed_negative 'seed with a traversal path' 'UPDATE NXU0009:' rename ../escape
seed_negative 'seed with an absolute path' 'UPDATE NXU0009:' rename /etc/passwd
seed_negative 'seed with a duplicated member' 'UPDATE NXU0009:' duplicate
seed_negative 'seed with a case-folded duplicate member' \
  'collide on case-insensitive media' casefold
seed_negative 'seed with a stray extra member' 'UPDATE NXU0012:' extra
seed_negative 'seed missing a declared member' 'UPDATE NXU0009:' drop
seed_negative 'seed with a wrong member count' 'UPDATE NXU0009:' miscount
seed_negative 'seed with a shifted offset' 'UPDATE NXU0009:' shift-offset
seed_negative 'seed with an unsupported mode' 'UPDATE NXU0009:' mode

# A FIFO standing in for the seed is never read as one.
install_personal_rezip "$GEN_A"
rm -f -- "$PORT_DIR/nxruntime-$GA_ID.nxb"
if mkfifo "$PORT_DIR/nxruntime-$GA_ID.nxb" 2>/dev/null; then
  runtime_runs_before=$(wc -l < "$RUN_MARKER")
  assert_refused 'a FIFO in place of the seed' \
    'UPDATE NXU0012: runtime seed nxruntime-'
  rm -f -- "$PORT_DIR/nxruntime-$GA_ID.nxb"
fi

# A symlink pointing at the real seed is not a regular file either.
install_personal_rezip "$GEN_A"
mv "$PORT_DIR/nxruntime-$GA_ID.nxb" "$PORT_DIR/seed.real"
ln -s seed.real "$PORT_DIR/nxruntime-$GA_ID.nxb"
runtime_runs_before=$(wc -l < "$RUN_MARKER")
assert_refused 'a symlink in place of the seed' \
  'UPDATE NXU0012: runtime seed nxruntime-'

# --------------------------------------------------- interruption before commit
# A staging tree left behind by a power loss is never adopted, healed or turned
# into a generation: the next launch rebuilds from the seed instead.
install_personal_rezip "$GEN_A"
mkdir -p "$PORT_DIR/.nxruntime/staging.999999.dead/$GA_ID/files/runtime"
printf '%s\n' "$GA_ID" > "$PORT_DIR/.nxruntime/staging.999999.dead/$GA_ID/commit"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "interrupted staging blocked the rebuild (exit $status)"
[[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
  fail 'the rebuild after an interrupted staging did not happen'
grep -Fq "UPDATE NXU0012: runtime cache rebuilt from seed" "$LOG" ||
  fail 'the rebuild after an interrupted staging has no receipt'

# ------------------------------------------------------- V3-STORAGE-01 on media
# The preflight receipt alone proves nothing: what matters is that a card
# without room REFUSES before writing, and that the refusal never damages the
# generation the owner is already running. Space is injected with a real small
# tmpfs, so the launcher measures a real filesystem rather than a mocked value.
mount_port_tmpfs() {
  local size=$1
  umount "$PORT_DIR" 2>/dev/null || true
  rm -rf -- "$PORT_DIR"
  mkdir -p "$PORT_DIR"
  mount -t tmpfs -o "size=$size,mode=0777" tmpfs "$PORT_DIR" ||
    fail "could not inject a $size filesystem under the port"
}
unmount_port_tmpfs() {
  umount "$PORT_DIR" 2>/dev/null || true
}

install_into_mounted_port() {
  local generated=$1
  rm -f -- "$LAUNCHER"
  ( cd "$generated/$PORT_ID" && find . -mindepth 1 \
      \( -name '.*' -prune \) -o -print0 ) |
    ( cd "$generated/$PORT_ID" && cpio -0 -pdm --quiet "$PORT_DIR" ) ||
    fail 'low-space fixture could not copy the port tree'
  cp -a "$generated/$LAUNCHER_NAME" "$LAUNCHER"
  mkdir -p "$PORT_DIR/gamedata"
  printf 'owner payload\n' > "$PORT_DIR/gamedata/owner.bin"
}

if mount_port_tmpfs 64M 2>/dev/null; then
  # Positive control FIRST: on a small but sufficient filesystem the same seed
  # materializes normally. Without this, the refusal below could be caused by
  # anything.
  install_into_mounted_port "$GEN_A"
  status=0; run_launcher || status=$?
  [[ $status == 0 ]] ||
    fail "the seed did not materialize on a sufficient filesystem ($status)"
  [[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
    fail 'the positive control did not build the generation'
  grep -Fq 'STORAGE: bundle=' "$LOG" ||
    fail 'the positive control produced no storage receipt'

  # Now the same bytes on a filesystem that cannot hold payload + margin.
  mount_port_tmpfs 4M
  install_into_mounted_port "$GEN_A"
  runtime_runs_before=$(wc -l < "$RUN_MARKER")
  status=0; run_launcher || status=$?
  [[ $status != 0 ]] || fail 'a filesystem without room still launched'
  grep -Fq 'UPDATE NXU0013: insufficient space to materialize the runtime' "$LOG" ||
    fail 'low space did not produce the NXU0013 refusal'
  grep -Fq 'STORAGE: bundle=' "$LOG" ||
    fail 'the low-space refusal carries no storage receipt'
  [[ ! -e $PORT_DIR/.nxruntime/generations/$GA_ID ]] ||
    fail 'the low-space refusal still created a generation'
  [[ -z $(ls -d "$PORT_DIR/.nxruntime"/staging.* 2>/dev/null) ]] ||
    fail 'the low-space refusal left staging behind'
  [[ $(wc -l < "$RUN_MARKER") -eq $runtime_runs_before ]] ||
    fail 'the low-space refusal reached the game'
  [[ -f $PORT_DIR/gamedata/owner.bin ]] ||
    fail 'the low-space refusal destroyed owner data'

  # ENOSPC must never disable the generation the owner already runs. Install A
  # healthy on a roomy filesystem, then let an update arrive with no room: the
  # update is refused and the ACTIVE generation still starts the game.
  mount_port_tmpfs 64M
  install_into_mounted_port "$GEN_A"
  status=0; run_launcher || status=$?
  [[ $status == 0 ]] || fail "the A baseline for the ENOSPC case exited $status"
  grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
    fail 'the ENOSPC baseline did not promote A'
  # Fill the card, leaving less than the margin free, then present seed B.
  cp -a "$GEN_B/$PORT_ID/nxruntime-$GB_ID.nxb" "$PORT_DIR/"
  cp -a "$GEN_B/$PORT_ID/nxport.json" "$PORT_DIR/nxport.json"
  cp -a "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
  for member in v4-bundle-loader port-env.sh extractor.json; do
    cp -a "$GEN_B/$PORT_ID/$member" "$PORT_DIR/$member"
  done
  cp -a "$GEN_B/$PORT_ID/lib/libprivate.so" "$PORT_DIR/lib/libprivate.so"
  cp -a "$GEN_B/$PORT_ID/nxextract/." "$PORT_DIR/nxextract/"
  # Leave a little room on purpose. A card at literally 0 bytes free cannot
  # even write log.txt, so the refusal would be silent and unobservable; the
  # case that matters in the field is a card with SOME space but not enough
  # for payload plus margin.
  ballast_kb=$(df -kP "$PORT_DIR" | awk 'NR==2 {print $4}')
  ballast_mb=$(( (ballast_kb - 1536) / 1024 ))
  [[ $ballast_mb -gt 0 ]] || fail 'the ENOSPC fixture has no room to fill'
  dd if=/dev/zero of="$PORT_DIR/gamedata/ballast" bs=1M count="$ballast_mb" \
    2>/dev/null || true
  runtime_runs_before=$(wc -l < "$RUN_MARKER")
  status=0; run_launcher || status=$?
  # The refusal happens, then the launcher correctly falls back to the healthy
  # ACTIVE generation and restarts from it -- which rotates log.txt. So the
  # NXU0013 receipt lives in the rotated log, and the run that finishes is the
  # old generation still working. Both facts are the point of this case.
  grep -Fq 'UPDATE NXU0013: insufficient space to materialize the runtime' \
    "$LOG" "${LOG%.txt}.prev.txt" 2>/dev/null ||
    fail 'the full card did not refuse the update with NXU0013'
  [[ ! -e $PORT_DIR/.nxruntime/generations/$GB_ID ]] ||
    fail 'the refused update still created generation B'
  [[ -f $PORT_DIR/.nxruntime/generations/$GA_ID/commit ]] ||
    fail 'ENOSPC damaged the generation the owner was already running'
  [[ -z $(ls -d "$PORT_DIR/.nxruntime"/staging.* 2>/dev/null) ]] ||
    fail 'the ENOSPC refusal left staging behind'
  [[ $status == 0 ]] ||
    fail "a refused update disabled the working generation (exit $status)"
  [[ $(tail -n 1 "$RUN_MARKER") == A ]] ||
    fail 'the fallback did not run the generation the owner already had'
  grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
    fail 'the refused update moved the active generation'
  # Free the card and prove the SAME install then completes the update: the
  # refusal was about space, never about the seed.
  rm -f -- "$PORT_DIR/gamedata/ballast"
  cp -a "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
  status=0; run_launcher || status=$?
  [[ $status == 0 ]] ||
    fail "the update did not complete once the card had room ($status)"
  [[ -f $PORT_DIR/.nxruntime/generations/$GB_ID/commit ]] ||
    fail 'B did not materialize once the card had room again'
  [[ $(tail -n 1 "$RUN_MARKER") == B ]] ||
    fail 'the completed update did not run the B closure'

  # A promotion that cannot be published must not leave a half generation
  # behind. The store is made READ-ONLY with a bind mount, not with chmod:
  # this suite runs root-mapped inside its namespace, and root walks straight
  # through permission bits, so a chmod-based injection would have proved
  # nothing at all.
  mount_port_tmpfs 64M
  install_into_mounted_port "$GEN_A"
  mkdir -p "$PORT_DIR/.nxruntime/generations"
  mount --bind "$PORT_DIR/.nxruntime/generations" \
    "$PORT_DIR/.nxruntime/generations" ||
    fail 'could not bind the generation store for the read-only case'
  mount -o remount,ro,bind "$PORT_DIR/.nxruntime/generations" ||
    fail 'could not make the generation store read-only'
  runtime_runs_before=$(wc -l < "$RUN_MARKER")
  status=0; run_launcher || status=$?
  umount "$PORT_DIR/.nxruntime/generations" 2>/dev/null || true
  [[ $status != 0 ]] || fail 'an unpublishable promotion still launched'
  [[ ! -e $PORT_DIR/.nxruntime/generations/$GA_ID ]] ||
    fail 'a failed promotion published a partial generation'
  [[ -z $(ls -d "$PORT_DIR/.nxruntime"/staging.* 2>/dev/null) ]] ||
    fail 'a failed promotion left staging behind'
  [[ $(wc -l < "$RUN_MARKER") -eq $runtime_runs_before ]] ||
    fail 'a failed promotion reached the game'

  unmount_port_tmpfs
  rm -rf -- "$PORT_DIR"
  STORAGE_INJECTED=1
else
  fail 'the low-space injection could not mount a tmpfs in this namespace'
fi

# ------------------------------------------------- header read, not full seed
# The seed is the whole port payload: hundreds of megabytes for a real game.
# Re-reading it end to end just to re-parse the header would pull all of that
# off the card on every rebuild, and streaming binary with embedded NUL bytes
# through sed is implementation-defined on a BusyBox CFW. The header parse must
# stop at the line boundary.
TEMPLATE=$PROJECT_ROOT/templates/launcher.sh.in
materialize_body=$(sed -n '/^nxbootstrap_bundle_materialize()/,/^}/p' \
  "$TEMPLATE")
[[ -n $materialize_body ]] ||
  fail 'the materialization function could not be located in the template'
if grep -Eq 'sed -n "[0-9]+,.*p" -- "\$bundle"' <<< "$materialize_body"; then
  fail 'the seed header is re-read by streaming the whole payload through sed'
fi
grep -Fq 'head -n "$((header_lines - 1))" -- "$bundle"' <<< "$materialize_body" ||
  fail 'the seed header is not read with a bounded head'


printf 'v4 bundle test: ALL PASS storage_injected=%s\n' "${STORAGE_INJECTED:-0}"
