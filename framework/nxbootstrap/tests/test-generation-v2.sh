#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Host gate for the schema-v3 generation_runtime closure. It boots real
# generated launchers and covers A/B promotion, torn immutable data, symlink
# healing, v1/v2 rollback separation, mandatory hashing, chmodless media and
# the fail-closed pending->active persistence boundary.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

command -v sha256sum >/dev/null 2>&1 || {
  printf 'generation-v2 test refused: sha256sum is required on the host\n' >&2
  exit 77
}

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-generation-v2.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-generation-v2.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'generation-v2 test failed: %s\n' "$*" >&2
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
CFW_NAME=generation-v2-fixture
sdl_controllerconfig=""
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

PORT_ID=generation-v2-port
LAUNCHER_NAME='Generation V2.sh'
PORT_DIR=$PORTS_DIR/$PORT_ID
LAUNCHER=$PORTS_DIR/$LAUNCHER_NAME
LOG=$PORT_DIR/log.txt
STATE=$PORT_DIR/.nxruntime/state.json
RUN_MARKER=$MARKERS/runtime-labels

write_runtime() {
  local root=$1 label=$2
  mkdir -p "$root/lib/runtime" "$root/nxextract/helpers" \
    "$root/nxextract/lib/aarch64" "$root/nxextract/specs"
  cat > "$root/generation-v2-loader" <<STUB
#!/bin/bash
[ "\${NXV2_HOOK_LABEL:-}" = "$label" ] || exit 12
printf '%s\n' '$label' >> "\$NXV2_RUN_MARKER"
if [ "\${NXV2_VIDEO_MODE:-ok}" = health-only ]; then
  printf 'runtime-evidence render-ready=1 audio-open=1 pid=%s video-proof=absent\n' \
    "\$\$"
else
  video_tmp="\$NXBOOTSTRAP_VIDEO_FILE.tmp.\$\$"
  (umask 077; set -C; printf '%s\n' \
    "{\"schema\":\"org.nextos.nxruntime.video-proof\",\"schema_version\":1,\"run_id\":\"\$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"\$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"\$NXBOOTSTRAP_HEALTH_PORT_ID\",\"verdict\":\"OK\",\"reason\":\"non-black\"}" \
    > "\$video_tmp") || exit 13
  mv -f "\$video_tmp" "\$NXBOOTSTRAP_VIDEO_FILE" || exit 13
  if [ "\${NXV2_VIDEO_MODE:-ok}" = ok-then-malformed ]; then
    # Leave enough time for the launcher to authenticate/log the initial OK,
    # then atomically revoke it before publishing health and exiting.
    sleep 2
    video_tmp="\$NXBOOTSTRAP_VIDEO_FILE.revoke.\$\$"
    (umask 077; set -C; printf '%s\n' 'revoked-after-ok' > "\$video_tmp") || exit 13
    mv -f "\$video_tmp" "\$NXBOOTSTRAP_VIDEO_FILE" || exit 13
  fi
fi
health_tmp="\$NXBOOTSTRAP_HEALTH_FILE.tmp.\$\$"
(umask 077; set -C; printf '%s\n' \
  "{\"schema\":\"\$NXBOOTSTRAP_HEALTH_SCHEMA\",\"schema_version\":\$NXBOOTSTRAP_HEALTH_SCHEMA_VERSION,\"run_id\":\"\$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"\$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"\$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" \
  > "\$health_tmp") || exit 13
mv -f "\$health_tmp" "\$NXBOOTSTRAP_HEALTH_FILE" || exit 13
if [ "\${NXV2_BREAK_STATE_WRITE:-0}" = 1 ]; then
  rm -f -- "\$NXCOMPAT_GAME_DIR_PHYSICAL/.nxruntime/state.json" || exit 14
  mkdir "\$NXCOMPAT_GAME_DIR_PHYSICAL/.nxruntime/state.json" || exit 14
fi
exit "\${NXV2_EXIT_STATUS:-0}"
STUB
  chmod 0755 "$root/generation-v2-loader"
  printf 'private library %s\n' "$label" > "$root/lib/libprivate.so"
  chmod 0644 "$root/lib/libprivate.so"
  printf 'NXExtract private library %s\n' "$label" \
    > "$root/nxextract/lib/aarch64/libfoo.so.1"
  chmod 0644 "$root/nxextract/lib/aarch64/libfoo.so.1"
  printf 'managed runtime data %s\n' "$label" \
    > "$root/lib/runtime/System.Private.CoreLib.dll"
  chmod 0644 "$root/lib/runtime/System.Private.CoreLib.dll"
  cat > "$root/port-env.sh" <<HOOK
export NXV2_HOOK_LABEL='$label'
HOOK
  chmod 0644 "$root/port-env.sh"
  printf '{"fixture":"generation-v2-nxextract","label":"%s"}\n' \
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
if spec != {"fixture": "generation-v2-nxextract", "label": "$label"}:
    raise SystemExit(41)
engine = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
if "LABEL=$label" not in engine:
    raise SystemExit(42)
PY
  chmod 0644 "$root/nxextract/helpers/prepare.py"
  printf '{"fixture":"generation-v2-nxextract","label":"%s"}\n' \
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
grep -Fq 'NXExtract private library $label' \
  "\$NXEXTRACT_GAME_DIR/nxextract/lib/aarch64/libfoo.so.1" || exit 45
python3 -B "\$NXEXTRACT_GAME_DIR/nxextract/helpers/prepare.py" \
  "\$NXEXTRACT_GAME_DIR/nxextract/specs/prepare.json" \
  "\$NXEXTRACT_GAME_DIR/nxextract/nxextract.py" || exit 46
printf '%s\n' '$label' >> "\$NXV2_NXEXTRACT_MARKER"
python3 -B - "\$NXEXTRACT_GAME_DIR/nxextract-result.json" <<'PYRESULT'
import json
import sys

result = {
    "schema": "org.nextos.nxextract.terminal-result",
    "schema_version": 1,
    "nxextract_version": "1.3.0",
    "outcome": "success",
    "code": "NXE0000",
    "final_phase": {"index": 8, "id": "ready", "label": "Ready"},
    "recipe": {"id": "generation-v2-nxextract", "version": "fixture-1",
               "digest": "0" * 64},
    "package_id": "org.nextos.generation_v2_nxextract",
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
  local target=$1 runtime_root=$2 title=$3 executable_hash library_hash nxlibrary_hash data_hash hook_hash
  local recipe_hash engine_hash runner_hash env_hash ui_hash helper_hash spec_hash
  executable_hash=$(sha256sum "$runtime_root/generation-v2-loader")
  executable_hash=${executable_hash%% *}
  library_hash=$(sha256sum "$runtime_root/lib/libprivate.so")
  library_hash=${library_hash%% *}
  nxlibrary_hash=$(sha256sum "$runtime_root/nxextract/lib/aarch64/libfoo.so.1")
  nxlibrary_hash=${nxlibrary_hash%% *}
  data_hash=$(sha256sum "$runtime_root/lib/runtime/System.Private.CoreLib.dll")
  data_hash=${data_hash%% *}
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
  "executable": "generation-v2-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "yes", "version": "1.3.0"},
  "required_files": ["generation-v2-loader", "lib/libprivate.so", "nxextract/lib/aarch64/libfoo.so.1", "port-env.sh"],
  "private_library_paths": ["lib", "nxextract/lib/aarch64"],
  "video_proof": "required",
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log",
  "generation_runtime": [
    {"role":"executable","path":"generation-v2-loader","mode":"0755","sha256":"$executable_hash"},
    {"role":"private-library","path":"lib/libprivate.so","mode":"0644","sha256":"$library_hash"},
    {"role":"private-library","path":"nxextract/lib/aarch64/libfoo.so.1","mode":"0644","sha256":"$nxlibrary_hash"},
    {"role":"runtime-data","path":"lib/runtime/System.Private.CoreLib.dll","mode":"0644","sha256":"$data_hash"},
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
write_manifest "$TEST_ROOT/nxport-a.json" "$RUNTIME_A" 'Generation V2 A'
write_manifest "$TEST_ROOT/nxport-b.json" "$RUNTIME_B" 'Generation V2 B'

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
spec = importlib.util.spec_from_file_location("generation_v2_fixture", generator_path)
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
  grep -Fq $'runtime-data\t0644\t' "$generation_root/components.v2" ||
    fail '0644 managed runtime data was not pinned'
  grep -Fq $'private-library\t0644\t'"$(sha256sum "$generated/$PORT_ID/nxextract/lib/aarch64/libfoo.so.1" | cut -d' ' -f1)"$'\tnxextract/lib/aarch64/libfoo.so.1' \
    "$generation_root/components.v2" ||
    fail '0644 NXExtract private library was not pinned'
  for nxrole in nxextract-recipe nxextract-engine nxextract-runner \
    nxextract-runtime-env nxextract-ui nxextract-helper nxextract-spec; do
    grep -Fq "$nxrole"$'\t' "$generation_root/components.v2" ||
      fail "$nxrole is absent from generation v2"
  done
  grep -Fq $'nxsplash\t0755\t' "$generation_root/components.v2" ||
    fail 'canonical NXSplash is absent from generation v2'
  [[ -f $generation_root/files/runtime/generation-v2-loader &&
     -f $generation_root/files/runtime/lib/libprivate.so &&
     -f $generation_root/files/runtime/nxextract/lib/aarch64/libfoo.so.1 &&
     -f $generation_root/files/runtime/lib/runtime/System.Private.CoreLib.dll &&
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

install_fresh() {
  local generated=$1
  rm -rf -- "$PORT_DIR" "$LAUNCHER"
  mkdir -p "$PORT_DIR"
  cp -a "$generated/$PORT_ID/." "$PORT_DIR/"
  cp -a "$generated/$LAUNCHER_NAME" "$LAUNCHER"
}

install_update() {
  local generated=$1 generation_id
  generation_id=$(read_generation_id "$generated")
  mkdir -p "$PORT_DIR/.nxruntime/generations"
  rm -rf -- "$PORT_DIR/.nxruntime/generations/$generation_id"
  cp -a "$generated/$PORT_ID/.nxruntime/generations/$generation_id" \
    "$PORT_DIR/.nxruntime/generations/"
  cp -a "$generated/$PORT_ID/nxport.json" "$PORT_DIR/nxport.json"
  cp -a "$generated/$PORT_ID/nxsplash-nextos" "$PORT_DIR/nxsplash-nextos"
  cp -a "$generated/$PORT_ID/generation-v2-loader" \
    "$PORT_DIR/generation-v2-loader"
  mkdir -p "$PORT_DIR/lib/runtime"
  cp -a "$generated/$PORT_ID/lib/libprivate.so" "$PORT_DIR/lib/libprivate.so"
  cp -a "$generated/$PORT_ID/lib/runtime/System.Private.CoreLib.dll" \
    "$PORT_DIR/lib/runtime/System.Private.CoreLib.dll"
  cp -a "$generated/$PORT_ID/port-env.sh" "$PORT_DIR/port-env.sh"
  cp -a "$generated/$PORT_ID/extractor.json" "$PORT_DIR/extractor.json"
  mkdir -p "$PORT_DIR/nxextract/helpers" "$PORT_DIR/nxextract/lib/aarch64" \
    "$PORT_DIR/nxextract/specs"
  cp -a "$generated/$PORT_ID/nxextract/nxextract.py" \
    "$PORT_DIR/nxextract/nxextract.py"
  cp -a "$generated/$PORT_ID/nxextract/run-extractor.sh" \
    "$PORT_DIR/nxextract/run-extractor.sh"
  cp -a "$generated/$PORT_ID/nxextract/nxextract-runtime-env.sh" \
    "$PORT_DIR/nxextract/nxextract-runtime-env.sh"
  cp -a "$generated/$PORT_ID/nxextract/nxextract-ui" \
    "$PORT_DIR/nxextract/nxextract-ui"
  cp -a "$generated/$PORT_ID/nxextract/helpers/prepare.py" \
    "$PORT_DIR/nxextract/helpers/prepare.py"
  cp -a "$generated/$PORT_ID/nxextract/lib/aarch64/libfoo.so.1" \
    "$PORT_DIR/nxextract/lib/aarch64/libfoo.so.1"
  cp -a "$generated/$PORT_ID/nxextract/specs/prepare.json" \
    "$PORT_DIR/nxextract/specs/prepare.json"
  cp -a "$generated/$LAUNCHER_NAME" "$LAUNCHER"
}

run_launcher() {
  local path_prefix=${1:-$PATH} status=0
  : > "$MARKERS/pm-finish"
  env -i PATH="$path_prefix" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    NXV2_RUN_MARKER="$RUN_MARKER" \
    NXV2_NXEXTRACT_MARKER="$MARKERS/nxextract-labels" \
    NXV2_BREAK_STATE_WRITE="${NXV2_BREAK_STATE_WRITE:-0}" \
    NXV2_VIDEO_MODE="${NXV2_VIDEO_MODE:-ok}" \
    NXV2_EXIT_STATUS="${NXV2_EXIT_STATUS:-0}" \
    NXV2_FAIL_STATE_TARGET="${NXV2_FAIL_STATE_TARGET:-}" \
    NXV2_FAIL_STAGE_SOURCE="${NXV2_FAIL_STAGE_SOURCE:-}" \
    NX_TEST_CHMODLESS_PREFIX="${NX_TEST_CHMODLESS_PREFIX:-}" \
    bash "$LAUNCHER" </dev/null >/dev/null 2>&1 || status=$?
  return "$status"
}

all_logs_contain() {
  grep -Fq -- "$1" "$PORT_DIR"/log*.txt 2>/dev/null
}

# A -> B: both exact runtime closures promote with the old v2 as anchor.
install_fresh "$GEN_A"
# An observed OK is not sticky: replacing its final inode with a malformed
# receipt before exit must invalidate the competing health-ready receipt.
NXV2_VIDEO_MODE=ok-then-malformed
status=0; run_launcher || status=$?
unset NXV2_VIDEO_MODE
[[ $status == 0 ]] || fail "revoked-video generation A exited $status"
grep -Fq 'VIDEO PROOF: run-bound OK reason=non-black' "$LOG" ||
  fail 'revoked-video negative never observed the initial OK'
grep -Fq "\"active\":\"null\"" "$STATE" ||
  fail 'malformed final video receipt promoted A from sticky OK'
grep -Fq "\"pending\":\"$GA_ID\"" "$STATE" ||
  fail 'malformed final video receipt lost the pending generation'
grep -Fq '"prehealth_failures":1' "$STATE" ||
  fail 'malformed final video receipt did not count a pre-health failure'
! grep -Fq 'UPDATE NXU0006:' "$LOG" ||
  fail 'malformed final video receipt emitted a promotion event'

# A health receipt, successful exit, live PID and audio/render log claims are
# not first-frame proof. With video_proof=required the pending generation must
# remain unpromoted until its exact run-bound OK receipt exists.
NXV2_VIDEO_MODE=health-only
status=0; run_launcher || status=$?
unset NXV2_VIDEO_MODE
[[ $status == 0 ]] || fail "health-only generation A exited $status"
grep -Fq "\"active\":\"null\"" "$STATE" ||
  fail 'health-only run promoted A without video OK'
grep -Fq "\"pending\":\"$GA_ID\"" "$STATE" ||
  fail 'health-only run lost the pending generation'
grep -Fq '"prehealth_failures":2' "$STATE" ||
  fail 'health-only run did not count the second pre-health failure'
grep -Fq 'runtime-evidence render-ready=1 audio-open=1 pid=' "$LOG" ||
  fail 'health-only negative did not exercise live/render/audio claims'
! grep -Fq 'UPDATE NXU0006:' "$LOG" ||
  fail 'health-only run emitted a promotion event'

# An exact health receipt and an exact final VIDEO OK do not absolve a child
# that returned nonzero.  The supervised status is part of health authority,
# so the pending generation remains unpromoted and the real status propagates.
NXV2_EXIT_STATUS=1
status=0; run_launcher || status=$?
unset NXV2_EXIT_STATUS
[[ $status == 1 ]] || fail "nonzero-ready generation A returned $status"
grep -Fq "\"active\":\"null\"" "$STATE" ||
  fail 'nonzero child with exact receipts promoted A'
grep -Fq "\"pending\":\"$GA_ID\"" "$STATE" ||
  fail 'nonzero child with exact receipts lost the pending generation'
grep -Fq '"prehealth_failures":3' "$STATE" ||
  fail 'nonzero child did not count the third pre-health failure'
grep -Fq 'UPDATE NXU0005: no valid run-bound health receipt' "$LOG" ||
  fail 'nonzero child lacks NXU0005 evidence'
! grep -Fq 'UPDATE NXU0006:' "$LOG" ||
  fail 'nonzero child with exact receipts emitted a promotion event'

status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "generation A exited $status"
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" || fail 'A was not promoted'
grep -Fq "UPDATE NXU0006: generation $GA_ID proved healthy" "$LOG" ||
  fail 'A promotion receipt is missing'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] || fail 'A executable/hook did not run'
[[ $(tail -n 1 "$MARKERS/nxextract-labels") == A ]] ||
  fail 'A NXExtract core/helper/spec closure did not run'

# A generation-v2 without its authenticated commit marker is incomplete. It
# must fail closed, preserve the missing marker and never reach NXExtract/game.
A_GENERATION=$PORT_DIR/.nxruntime/generations/$GA_ID
A_COMMIT=$A_GENERATION/commit
runtime_runs_before=$(wc -l < "$RUN_MARKER")
nxextract_runs_before=$(wc -l < "$MARKERS/nxextract-labels")
rm -f -- "$A_COMMIT"
status=0; run_launcher || status=$?
[[ $status != 0 ]] || fail 'generation-v2 without commit marker launched'
[[ ! -e $A_COMMIT && ! -L $A_COMMIT ]] ||
  fail 'missing generation-v2 commit marker was recreated'
grep -Fq "UPDATE NXU0002: generation $GA_ID is truncated or stale; ignored (not deleted)" "$LOG" ||
  fail 'missing generation-v2 commit marker lacks NXU0002 evidence'
grep -Fq 'UPDATE NXU0009: no complete generation-v2 closure found; launch refused' "$LOG" ||
  fail 'missing generation-v2 commit marker lacks NXU0009 evidence'
[[ $(wc -l < "$RUN_MARKER") -eq $runtime_runs_before &&
   $(wc -l < "$MARKERS/nxextract-labels") -eq $nxextract_runs_before ]] ||
  fail 'missing generation-v2 commit marker reached extraction/game'
cp -a "$GEN_A/$PORT_ID/.nxruntime/generations/$GA_ID/commit" "$A_COMMIT"

# Two-pass healing is transaction-like before publication: if a late selected
# member cannot be staged, every earlier temporary is removed and no runtime
# root byte changes. This is the host proof for all-or-nothing prepublication.
install_update "$GEN_B"
for target in generation-v2-loader lib/libprivate.so \
  lib/runtime/System.Private.CoreLib.dll port-env.sh \
  extractor.json nxextract/nxextract.py nxextract/run-extractor.sh \
  nxextract/nxextract-runtime-env.sh nxextract/nxextract-ui \
  nxextract/helpers/prepare.py nxextract/lib/aarch64/libfoo.so.1 \
  nxextract/specs/prepare.json nxsplash-nextos; do
  cp -a "$GEN_A/$PORT_ID/$target" "$PORT_DIR/$target"
done
FAKE_STAGE=$TEST_ROOT/fake-stage
mkdir "$FAKE_STAGE"
cat > "$FAKE_STAGE/cat" <<'SH'
#!/bin/sh
[ "${1:-}" != "$NXV2_FAIL_STAGE_SOURCE" ] || exit 1
exec /usr/bin/cat "$@"
SH
chmod 0755 "$FAKE_STAGE/cat"
NXV2_FAIL_STAGE_SOURCE="$PORT_DIR/.nxruntime/generations/$GB_ID/files/runtime/nxextract/specs/prepare.json"
nxextract_runs_before=$(wc -l < "$MARKERS/nxextract-labels")
game_runs_before=$(wc -l < "$RUN_MARKER")
status=0; run_launcher "$FAKE_STAGE:$PATH" || status=$?
NXV2_FAIL_STAGE_SOURCE=''
[[ $status != 0 ]] || fail 'late NXExtract staging failure launched'
for target in generation-v2-loader lib/libprivate.so \
  lib/runtime/System.Private.CoreLib.dll port-env.sh \
  extractor.json nxextract/nxextract.py nxextract/run-extractor.sh \
  nxextract/nxextract-runtime-env.sh nxextract/nxextract-ui \
  nxextract/helpers/prepare.py nxextract/lib/aarch64/libfoo.so.1 \
  nxextract/specs/prepare.json nxsplash-nextos; do
  cmp -s "$PORT_DIR/$target" "$GEN_A/$PORT_ID/$target" ||
    fail "staging failure published a partial member $target"
done
! find "$PORT_DIR" -name '*.nxheal.*' -print -quit | grep -q . ||
  fail 'staging failure left a heal temporary'
[[ $(wc -l < "$MARKERS/nxextract-labels") -eq $nxextract_runs_before &&
   $(wc -l < "$RUN_MARKER") -eq $game_runs_before ]] ||
  fail 'staging failure reached extraction/game'

install_update "$GEN_B"
# Reproduce a real overlay update stopped halfway through its root copies:
# launcher/recipe/runner/UI/spec are B, while engine/runtime-env/imported helper
# are still A. The immutable B closure must heal the entire set before NXExtract.
cp -a "$GEN_A/$PORT_ID/nxextract/nxextract.py" \
  "$PORT_DIR/nxextract/nxextract.py"
cp -a "$GEN_A/$PORT_ID/nxextract/nxextract-runtime-env.sh" \
  "$PORT_DIR/nxextract/nxextract-runtime-env.sh"
cp -a "$GEN_A/$PORT_ID/nxextract/helpers/prepare.py" \
  "$PORT_DIR/nxextract/helpers/prepare.py"
cp -a "$GEN_A/$PORT_ID/nxextract/lib/aarch64/libfoo.so.1" \
  "$PORT_DIR/nxextract/lib/aarch64/libfoo.so.1"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "generation B exited $status"
grep -Fq "\"active\":\"$GB_ID\"" "$STATE" || fail 'B was not promoted'
grep -Fq "\"previous_healthy\":\"$GA_ID\"" "$STATE" ||
  fail 'A was not retained as the v2 rollback anchor'
[[ $(tail -n 1 "$RUN_MARKER") == B ]] || fail 'B executable/hook did not run'
[[ $(tail -n 1 "$MARKERS/nxextract-labels") == B ]] ||
  fail 'hybrid NXExtract root was not healed entirely to B'
for target in extractor.json nxextract/nxextract.py \
  nxextract/run-extractor.sh nxextract/nxextract-runtime-env.sh \
  nxextract/nxextract-ui nxextract/helpers/prepare.py \
  nxextract/lib/aarch64/libfoo.so.1 nxextract/specs/prepare.json; do
  cmp -s "$PORT_DIR/$target" "$GEN_B/$PORT_ID/$target" ||
    fail "hybrid update left stale NXExtract member $target"
done

# An interrupted staging pass may leave only its reserved adjacent temporary.
# A subsequent launch removes a regular dead-PID temporary, revalidates the
# exact tree and then runs; arbitrary undeclared names remain fail-closed below.
STALE_HEAL="$PORT_DIR/nxextract/helpers/prepare.py.nxheal.99999999.1"
STALE_LIBRARY_HEAL="$PORT_DIR/nxextract/lib/aarch64/libfoo.so.1.nxheal.99999999.2"
OWNER_OUTSIDE="$PORT_DIR/generation-v2-loader.nxheal.99999999.1"
printf 'interrupted staging\n' > "$STALE_HEAL"
printf 'interrupted library staging\n' > "$STALE_LIBRARY_HEAL"
printf 'owner sentinel\n' > "$OWNER_OUTSIDE"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "interrupted-stage recovery exited $status"
[[ ! -e $STALE_HEAL && ! -L $STALE_HEAL ]] ||
  fail 'dead-PID heal temporary survived recovery'
[[ ! -e $STALE_LIBRARY_HEAL && ! -L $STALE_LIBRARY_HEAL ]] ||
  fail 'dead-PID NXExtract private-library temporary survived recovery'
grep -Fqx 'owner sentinel' "$OWNER_OUTSIDE" ||
  fail 'dead-PID-shaped path outside NXExtract was not preserved'
[[ $(tail -n 1 "$MARKERS/nxextract-labels") == B ]] ||
  fail 'interrupted-stage recovery did not run the exact B closure'

# A torn imported helper in immutable B is ignored; complete A heals every root
# member and runs. A helper/spec mismatch must be caught before NXExtract starts.
B_GENERATION=$PORT_DIR/.nxruntime/generations/$GB_ID
TORN_HELPER="$B_GENERATION/files/runtime/nxextract/helpers/prepare.py"
find "$B_GENERATION/files" -type f -exec chmod 0777 {} +
printf 'torn\n' >> "$TORN_HELPER"
cp -a "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
# 0.7.5: a healthy committed boot is O(1) and does not reread the immutable
# store; the torn member legitimately surfaces at the next deep event. Arm the
# one-shot deep-verify marker (the same switch an unhealthy run arms) so this
# launch takes the deep path and must still diagnose the mismatch.
: > "$PORT_DIR/.nxruntime/deep-verify-next"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "torn-generation fallback exited $status"
all_logs_contain "launcher carries $GB_ID, runtime generation is $GA_ID" ||
  fail 'torn generation was not diagnosed as a generation mismatch'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] || fail 'torn B did not fall back to A'
[[ $(tail -n 1 "$MARKERS/nxextract-labels") == A ]] ||
  fail 'torn NXExtract helper reached the extraction phase'
python3 - "$B_GENERATION/files" <<'PY' ||
import os
import stat
import sys

root = sys.argv[1]
members = [
    os.path.join(directory, name)
    for directory, _subdirs, names in os.walk(root)
    for name in names
]
if not members:
    raise SystemExit("generation-v2 store fixture has no members")
changed = [
    path for path in members
    if stat.S_IMODE(os.stat(path, follow_symlinks=False).st_mode) != 0o777
]
if changed:
    raise SystemExit("mode changed before complete authentication: " + changed[0])
PY
  fail 'unauthenticated torn generation normalized a member before full validation'
rm -rf -- "$B_GENERATION"
cp -a "$GEN_B/$PORT_ID/.nxruntime/generations/$GB_ID" \
  "$PORT_DIR/.nxruntime/generations/"

# A stale undeclared spec from an older overlay is not deleted or consumed.
# Exact live-tree closure fails before runner/game, preventing an imported file
# outside the selected generation from completing a hybrid NXExtract run.
install_update "$GEN_B"
printf '{"stale":true}\n' > "$PORT_DIR/nxextract/specs/stale.json"
nxextract_runs_before=$(wc -l < "$MARKERS/nxextract-labels")
game_runs_before=$(wc -l < "$RUN_MARKER")
status=0; run_launcher || status=$?
[[ $status != 0 ]] || fail 'undeclared NXExtract spec was accepted'
[[ $(wc -l < "$MARKERS/nxextract-labels") -eq $nxextract_runs_before &&
   $(wc -l < "$RUN_MARKER") -eq $game_runs_before ]] ||
  fail 'undeclared NXExtract spec reached extraction/game'
grep -Fq 'UPDATE NXU0009: generation-v2 closure could not be healed and revalidated' "$LOG" ||
  fail 'undeclared NXExtract spec lacks fail-closed update evidence'
rm -f -- "$PORT_DIR/nxextract/specs/stale.json"

# The component allowlist is itself bound to identity.json. Appending a valid
# looking duplicate cannot redefine or extend the runtime closure.
tail -n 1 "$B_GENERATION/components.v2" >> "$B_GENERATION/components.v2"
cp -a "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "metadata-tamper fallback exited $status"
[[ $(tail -n 1 "$RUN_MARKER") == A ]] ||
  fail 'tampered component allowlist was accepted as B'
rm -rf -- "$B_GENERATION"
cp -a "$GEN_B/$PORT_ID/.nxruntime/generations/$GB_ID" \
  "$PORT_DIR/.nxruntime/generations/"

# Crash-loop rollback replaces hostile top-level symlinks; it never follows
# them and revalidates the complete A closure before restarting A's launcher.
install_update "$GEN_B"
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"$GA_ID\",\"pending\":\"$GB_ID\",\"previous_healthy\":\"$GA_ID\",\"activation_seq\":7,\"prehealth_failures\":3,\"failure_generation\":\"$GB_ID\",\"last_health_run_id\":\"prior\"}" \
  > "$STATE"
for target in generation-v2-loader lib/libprivate.so \
  lib/runtime/System.Private.CoreLib.dll port-env.sh \
  extractor.json nxextract/nxextract.py nxextract/run-extractor.sh \
  nxextract/nxextract-runtime-env.sh nxextract/nxextract-ui \
  nxextract/helpers/prepare.py nxextract/lib/aarch64/libfoo.so.1 \
  nxextract/specs/prepare.json nxsplash-nextos; do
  external=$TEST_ROOT/external-${target//\//-}
  printf 'external sentinel %s\n' "$target" > "$external"
  rm -f -- "$PORT_DIR/$target"
  ln -s "$external" "$PORT_DIR/$target"
done
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "symlink rollback exited $status"
all_logs_contain "previous healthy generation $GA_ID selected" ||
  fail 'crash-loop did not select A'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] || fail 'rollback did not execute A'
for target in generation-v2-loader lib/libprivate.so \
  lib/runtime/System.Private.CoreLib.dll port-env.sh \
  extractor.json nxextract/nxextract.py nxextract/run-extractor.sh \
  nxextract/nxextract-runtime-env.sh nxextract/nxextract-ui \
  nxextract/helpers/prepare.py nxextract/lib/aarch64/libfoo.so.1 \
  nxextract/specs/prepare.json nxsplash-nextos; do
  [[ -f $PORT_DIR/$target && ! -L $PORT_DIR/$target ]] ||
    fail "rollback left symlink $target"
  grep -Fq 'external sentinel' "$TEST_ROOT/external-${target//\//-}" ||
    fail "rollback wrote through external symlink $target"
done

# A valid legacy generation remains readable by old launchers, but a pending
# v2 generation must never preserve it as previous_healthy.
cat > "$TEST_ROOT/legacy.json" <<JSON
{
  "schema_version": 2,
  "id": "$PORT_ID",
  "title": "Legacy control generation",
  "launcher_name": "$LAUNCHER_NAME",
  "architecture": "aarch64",
  "executable": "generation-v2-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode":"no","version":"1.3.0"},
  "required_files": ["generation-v2-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
LEGACY_OUT=$TEST_ROOT/generated-legacy
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/legacy.json" \
  --output "$LEGACY_OUT" >/dev/null || fail 'legacy generator fixture failed'
LEGACY_ID=$(read_generation_id "$LEGACY_OUT")
cp -a "$LEGACY_OUT/$PORT_ID/.nxruntime/generations/$LEGACY_ID" \
  "$PORT_DIR/.nxruntime/generations/"
install_update "$GEN_B"
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"$LEGACY_ID\",\"pending\":\"null\",\"previous_healthy\":\"$LEGACY_ID\",\"activation_seq\":8,\"prehealth_failures\":0,\"failure_generation\":\"null\",\"last_health_run_id\":\"legacy\"}" \
  > "$STATE"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "v1-to-v2 migration exited $status"
grep -Fq "\"active\":\"$GB_ID\"" "$STATE" || fail 'B was not promoted over v1'
grep -Fq '"previous_healthy":"null"' "$STATE" ||
  fail 'legacy v1 became a rollback anchor for pending v2'

# A v2 launcher cannot validate any generation when sha256sum is unusable.
FAKE_SHA=$TEST_ROOT/fake-sha
mkdir "$FAKE_SHA"
cat > "$FAKE_SHA/sha256sum" <<'SH'
#!/bin/sh
exit 127
SH
chmod 0755 "$FAKE_SHA/sha256sum"
status=0; run_launcher "$FAKE_SHA:$PATH" || status=$?
[[ $status != 0 ]] || fail 'v2 launched without a usable sha256sum'
grep -Fq 'UPDATE NXU0009:' "$LOG" || fail 'missing-hash failure lacks NXU0009'

# NXU0006 is forbidden if the final active-state rename cannot persist.
install_update "$GEN_B"
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"$GB_ID\",\"pending\":\"null\",\"previous_healthy\":\"null\",\"activation_seq\":9,\"prehealth_failures\":0,\"failure_generation\":\"null\",\"last_health_run_id\":\"prior\"}" \
  > "$STATE"
FAKE_STATE=$TEST_ROOT/fake-state
mkdir "$FAKE_STATE"
cat > "$FAKE_STATE/mv" <<'SH'
#!/bin/sh
target=''
for value in "$@"; do target=$value; done
[ "$target" != "$NXV2_FAIL_STATE_TARGET" ] || exit 1
exec /usr/bin/mv "$@"
SH
chmod 0755 "$FAKE_STATE/mv"
NXV2_FAIL_STATE_TARGET=$STATE
status=0; run_launcher "$FAKE_STATE:$PATH" || status=$?
NXV2_FAIL_STATE_TARGET=''
[[ $status == 70 ]] || fail "state persistence failure returned $status, expected 70"
grep -Fq 'UPDATE NXU0011: valid health receipt observed but active state was not persisted' "$LOG" ||
  fail 'state persistence failure lacks NXU0011'
! grep -Fq 'UPDATE NXU0006:' "$LOG" ||
  fail 'NXU0006 was emitted despite failed active-state persistence'

# PortMaster POSIX fixture: HarbourMaster applies chmod -R 777 to the complete
# extracted port. The authenticated store must be normalized before live heal,
# and both store/live must end with the exact manifest modes.
install_fresh "$GEN_A"
chmod -R 0777 "$PORT_DIR"
chmod 0777 "$LAUNCHER"
# 0.7.5: the healthy O(1) boot leaves POSIX modes for the next deep event
# instead of paying an O(N) restore on every launch. Arm the one-shot marker
# so this launch takes the deep path and must still restore the exact modes.
: > "$PORT_DIR/.nxruntime/deep-verify-next"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "PortMaster POSIX 0777 fixture exited $status"
! grep -Fq 'truncated or stale' "$LOG" ||
  fail 'PortMaster POSIX 0777 generation was rejected as stale'
! grep -Fq 'UPDATE NXU0009:' "$LOG" ||
  fail 'PortMaster POSIX 0777 generation produced NXU0009'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'PortMaster POSIX 0777 generation was not promoted'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] ||
  fail 'PortMaster POSIX 0777 generation did not execute its exact runtime'
python3 - "$PORT_DIR/.nxruntime/generations/$GA_ID" "$PORT_DIR" "$LAUNCHER" <<'PY' ||
import os
import stat
import sys

generation, port_dir, launcher = sys.argv[1:]
with open(os.path.join(generation, "components.v2"), encoding="utf-8") as source:
    for line in source:
        role, mode_text, _digest, path = line.rstrip("\n").split("\t")
        if role == "launcher":
            store_path = os.path.join(generation, "files", "launcher", path)
            live_path = launcher
        elif role == "nxport":
            store_path = os.path.join(generation, "files", "nxport.json")
            live_path = os.path.join(port_dir, "nxport.json")
        else:
            store_path = os.path.join(generation, "files", "runtime", path)
            live_path = os.path.join(port_dir, path)
        expected = int(mode_text, 8)
        for tree, candidate in (("store", store_path), ("live", live_path)):
            actual = stat.S_IMODE(os.stat(candidate, follow_symlinks=False).st_mode)
            if actual != expected:
                raise SystemExit(
                    f"{tree} mode mismatch for {path}: {actual:04o} != {expected:04o}"
                )
PY
  fail 'PortMaster POSIX 0777 modes were not restored exactly'

# FAT/exFAT/FUSE fixture: every component reports 0777 and chmod is ignored.
# Hash, regular-file and no-symlink gates remain active, and the executable is
# still executable. The probe must distinguish this from a normal POSIX mode
# mismatch instead of rejecting a valid ArkOS installation.
install_fresh "$GEN_A"
find "$PORT_DIR/.nxruntime/generations/$GA_ID/files" -type f -exec chmod 0777 {} +
chmod 0777 "$LAUNCHER" "$PORT_DIR/nxport.json" "$PORT_DIR/nxsplash-nextos" \
  "$PORT_DIR/generation-v2-loader" "$PORT_DIR/lib/libprivate.so" \
  "$PORT_DIR/lib/runtime/System.Private.CoreLib.dll" \
  "$PORT_DIR/port-env.sh" "$PORT_DIR/extractor.json" \
  "$PORT_DIR/nxextract/nxextract.py" \
  "$PORT_DIR/nxextract/run-extractor.sh" \
  "$PORT_DIR/nxextract/nxextract-runtime-env.sh" \
  "$PORT_DIR/nxextract/nxextract-ui" \
  "$PORT_DIR/nxextract/helpers/prepare.py" \
  "$PORT_DIR/nxextract/lib/aarch64/libfoo.so.1" \
  "$PORT_DIR/nxextract/specs/prepare.json"
FAKE_MODE=$TEST_ROOT/fake-mode
mkdir "$FAKE_MODE"
cat > "$FAKE_MODE/chmod" <<'SH'
#!/bin/sh
for value in "$@"; do
  case "$value" in
    "$NX_TEST_CHMODLESS_PREFIX"/*) exit 0 ;;
  esac
done
exec /usr/bin/chmod "$@"
SH
cat > "$FAKE_MODE/ls" <<'SH'
#!/bin/sh
if [ "${1:-}" = -Lld ]; then
  shift
  [ "${1:-}" = -- ] && shift
  case "${1:-}" in
    "$NX_TEST_CHMODLESS_PREFIX"/*)
      printf '%s\n' '-rwxrwxrwx 1 fixture fixture 1 Jan 1 00:00 synthetic'
      exit 0
      ;;
  esac
fi
exec /usr/bin/ls "$@"
SH
chmod 0755 "$FAKE_MODE/chmod" "$FAKE_MODE/ls"
NX_TEST_CHMODLESS_PREFIX=$PORTS_DIR
status=0; run_launcher "$FAKE_MODE:$PATH" || status=$?
NX_TEST_CHMODLESS_PREFIX=''
[[ $status == 0 ]] || fail "chmodless 0777 fixture exited $status"
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'chmodless generation was not promoted'
[[ $(tail -n 1 "$RUN_MARKER") == A ]] ||
  fail 'chmodless generation did not execute its exact runtime'

printf 'nxbootstrap generation-v2 gate passed: ab=1 video-required=1 video-ok-revoked=1 nonzero-ready-no-promote=1 v2_commit_required=1 nxextract_hybrid=1 nxextract_private_library=1 nxextract_extra=1 staging=1 interrupted_stage=1 owner_outside=1 torn=1 unauthenticated_no_chmod=1 symlink=1 legacy_control_only=1 sha_required=1 state_fail_closed=1 portmaster_posix_0777=1 chmodless_0777=1\n'
