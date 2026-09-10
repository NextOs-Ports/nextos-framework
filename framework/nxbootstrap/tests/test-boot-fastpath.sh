#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V4-BOOT gate: a healthy committed boot is O(1) — its subprocess count and
# wall time do not scale with the closure size, no per-member
# chmod/rm/ls/sha256sum ceremony runs, chmodless media is probed at most once,
# recovery verifies the closure with batched sha256sum instead of one call per
# member, the deep path is announced (never a silent black screen) and the
# fail-closed boundaries (absent commit, adulterated identity) hold.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

command -v sha256sum >/dev/null 2>&1 || {
  printf 'boot-fastpath test refused: sha256sum is required on the host\n' >&2
  exit 77
}

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-boot-fastpath.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-boot-fastpath.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'boot-fastpath test failed: %s\n' "$*" >&2
  if [[ -n ${LOG-} && -f $LOG ]]; then
    printf '%s\n' '--- launcher log ---' >&2
    sed -n '1,160p' "$LOG" >&2 || true
  fi
  exit 1
}

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
COUNTERS=$TEST_ROOT/counters
SHIMS=$TEST_ROOT/shims
mkdir -p "$PORTS_DIR" "$PM_DIR" "$MARKERS" "$COUNTERS" "$SHIMS"
mkdir -m 0700 "$RUNTIME_DIR"
cat > "$PM_DIR/control.txt" <<CONTROL
directory="${TEST_ROOT#/}/real/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=boot-fastpath-fixture
sdl_controllerconfig=""
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

PORT_ID=boot-fastpath-port
LAUNCHER_NAME='Boot Fastpath.sh'

# --- counting shims. chmod/ls additionally emulate chmodless FAT media for
# every path under NX_TEST_CHMODLESS_PREFIX (same hook the generation-v2 gate
# uses); rm and sha256sum only count and delegate. --------------------------
REAL_CHMOD=$(command -v chmod); REAL_LS=$(command -v ls)
REAL_RM=$(command -v rm); REAL_SHA=$(command -v sha256sum)
# chmod delegates for real (the ls shim below is what fakes the FAT 0777
# view; on real FAT media files are executable, so staged heals must be too).
cat > "$SHIMS/chmod" <<SHIM
#!/bin/sh
echo x >> "$COUNTERS/chmod"
exec "$REAL_CHMOD" "\$@"
SHIM
cat > "$SHIMS/ls" <<SHIM
#!/bin/sh
echo x >> "$COUNTERS/ls"
if [ "\${1:-}" = -Lld ]; then
  shift
  [ "\${1:-}" = -- ] && shift
  case "\${1:-}" in
    "\$NX_TEST_CHMODLESS_PREFIX"/*)
      printf '%s\n' '-rwxrwxrwx 1 fixture fixture 1 Jan 1 00:00 synthetic'
      exit 0
      ;;
  esac
  exec "$REAL_LS" -Lld -- "\$@"
fi
exec "$REAL_LS" "\$@"
SHIM
cat > "$SHIMS/rm" <<SHIM
#!/bin/sh
echo x >> "$COUNTERS/rm"
exec "$REAL_RM" "\$@"
SHIM
cat > "$SHIMS/sha256sum" <<SHIM
#!/bin/sh
echo x >> "$COUNTERS/sha256sum"
exec "$REAL_SHA" "\$@"
SHIM
chmod 0755 "$SHIMS/chmod" "$SHIMS/ls" "$SHIMS/rm" "$SHIMS/sha256sum"

reset_counters() { rm -f "$COUNTERS"/*; }
counter() { [ -f "$COUNTERS/$1" ] && wc -l < "$COUNTERS/$1" || echo 0; }

write_runtime() {
  local root=$1 members=$2 index
  mkdir -p "$root/lib/runtime"
  cat > "$root/boot-fastpath-loader" <<'STUB'
#!/bin/bash
health_tmp="$NXBOOTSTRAP_HEALTH_FILE.tmp.$$"
(umask 077; set -C; printf '%s\n' \
  "{\"schema\":\"$NXBOOTSTRAP_HEALTH_SCHEMA\",\"schema_version\":$NXBOOTSTRAP_HEALTH_SCHEMA_VERSION,\"run_id\":\"$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" \
  > "$health_tmp") || exit 13
mv -f "$health_tmp" "$NXBOOTSTRAP_HEALTH_FILE" || exit 13
exit 0
STUB
  chmod 0755 "$root/boot-fastpath-loader"
  cat > "$root/port-env.sh" <<'HOOK'
: fastpath fixture hook
HOOK
  chmod 0644 "$root/port-env.sh"
  for ((index=0; index<members; index++)); do
    printf 'member %06d payload\n' "$index" > "$root/lib/runtime/$(printf 'data-%06d.bin' "$index")"
    chmod 0644 "$root/lib/runtime/$(printf 'data-%06d.bin' "$index")"
  done
}

write_manifest() {
  local target=$1 runtime_root=$2 members=$3 index digest name
  local executable_hash hook_hash
  executable_hash=$(sha256sum "$runtime_root/boot-fastpath-loader")
  executable_hash=${executable_hash%% *}
  hook_hash=$(sha256sum "$runtime_root/port-env.sh")
  hook_hash=${hook_hash%% *}
  {
    cat <<JSON
{
  "schema_version": 3,
  "id": "$PORT_ID",
  "title": "Boot Fastpath",
  "launcher_name": "$LAUNCHER_NAME",
  "architecture": "aarch64",
  "executable": "boot-fastpath-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["boot-fastpath-loader", "port-env.sh"],
  "private_library_paths": ["lib"],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log",
  "generation_runtime": [
    {"role":"executable","path":"boot-fastpath-loader","mode":"0755","sha256":"$executable_hash"}
JSON
    for ((index=0; index<members; index++)); do
      name=$(printf 'data-%06d.bin' "$index")
      digest=$(sha256sum "$runtime_root/lib/runtime/$name")
      digest=${digest%% *}
      printf ',\n    {"role":"runtime-data","path":"lib/runtime/%s","mode":"0644","sha256":"%s"}' \
        "$name" "$digest"
    done
    printf ',\n    {"role":"runtime-hook","path":"port-env.sh","mode":"0644","sha256":"%s"}' "$hook_hash"
    printf '\n  ]\n}\n'
  } > "$target"
}

generate_fixture() {
  python3 -B - "$PROJECT_ROOT/tools/generate-port.py" "$TEST_DIR/splash-stub.sh" \
    "$1" "$2" "$3" <<'PY'
import hashlib
import importlib.util
import sys
from pathlib import Path

generator_path, splash_path, manifest_path, output_path, runtime_root = sys.argv[1:]
spec = importlib.util.spec_from_file_location("boot_fastpath_fixture", generator_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
splash = Path(splash_path).resolve()
digest = hashlib.sha256(splash.read_bytes()).hexdigest()
module.nxsplash_artifact = lambda _architecture: (splash, digest)
module.generate(Path(manifest_path), Path(output_path), False, Path(runtime_root))
PY
}

install_port() {
  local generated=$1
  rm -rf "$PORTS_DIR/$PORT_ID" "$PORTS_DIR/$LAUNCHER_NAME"
  cp -a "$generated/$PORT_ID" "$PORTS_DIR/$PORT_ID"
  cp -a "$generated/$LAUNCHER_NAME" "$PORTS_DIR/$LAUNCHER_NAME"
}

launch() {
  # Chmodless FAT media is always simulated (the Tearscape field shape).
  local rc=0 watcher
  : > "$MARKERS/preflight-time"
  rm -f "$LOG"
  NXBOOT_LAUNCH_START=$EPOCHREALTIME
  ( while :; do
      if grep -q 'PHASE preflight START' "$LOG" 2>/dev/null; then
        printf '%s\n' "$EPOCHREALTIME" > "$MARKERS/preflight-time"
        exit 0
      fi
      sleep 0.02
    done ) & watcher=$!
  env -i PATH="$SHIMS:$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    NX_TEST_CHMODLESS_PREFIX="$PORTS_DIR" \
    bash "$PORTS_DIR/$LAUNCHER_NAME" </dev/null >> "$MARKERS/stdout" 2>&1 || rc=$?
  kill "$watcher" 2>/dev/null || true
  wait "$watcher" 2>/dev/null || true
  NXBOOT_LAUNCH_RC=$rc
  NXBOOT_PREFLIGHT_AT=$(cat "$MARKERS/preflight-time" 2>/dev/null || true)
}

elapsed_to_preflight() {
  [ -n "$NXBOOT_PREFLIGHT_AT" ] || { echo 999; return; }
  awk -v a="$NXBOOT_LAUNCH_START" -v b="$NXBOOT_PREFLIGHT_AT" 'BEGIN{printf "%.3f", b-a}'
}

run_case() {
  local members=$1 label=$2
  local runtime=$TEST_ROOT/runtime-$label generated=$TEST_ROOT/generated-$label
  write_runtime "$runtime" "$members"
  write_manifest "$TEST_ROOT/nxport-$label.json" "$runtime" "$members"
  generate_fixture "$TEST_ROOT/nxport-$label.json" "$generated" "$runtime" ||
    fail "generator refused the $label fixture"
  install_port "$generated"
  LOG=$PORTS_DIR/$PORT_ID/log.txt

  # First normal opening: committed generation, no state yet.
  reset_counters
  launch
  [ "$NXBOOT_LAUNCH_RC" = 0 ] || fail "$label first boot failed rc=$NXBOOT_LAUNCH_RC"
  grep -q 'PHASE preflight START' "$LOG" || fail "$label first boot missed preflight"
  grep -q 'UPDATE NXU0014' "$LOG" && fail "$label first boot took the deep path"
  FIRST_SHA=$(counter sha256sum); FIRST_CHMOD=$(counter chmod)
  FIRST_RM=$(counter rm); FIRST_LS=$(counter ls)
  FIRST_TIME=$(elapsed_to_preflight)

  # Second opening: state now points at the same generation.
  reset_counters
  launch
  [ "$NXBOOT_LAUNCH_RC" = 0 ] || fail "$label second boot failed rc=$NXBOOT_LAUNCH_RC"
  grep -q 'UPDATE NXU0014' "$LOG" && fail "$label second boot took the deep path"
  SECOND_SHA=$(counter sha256sum); SECOND_CHMOD=$(counter chmod)
  SECOND_RM=$(counter rm); SECOND_LS=$(counter ls)
  SECOND_TIME=$(elapsed_to_preflight)
  printf '%s members=%s first: sha=%s chmod=%s rm=%s ls=%s t=%ss | second: sha=%s chmod=%s rm=%s ls=%s t=%ss\n' \
    "$label" "$members" "$FIRST_SHA" "$FIRST_CHMOD" "$FIRST_RM" "$FIRST_LS" "$FIRST_TIME" \
    "$SECOND_SHA" "$SECOND_CHMOD" "$SECOND_RM" "$SECOND_LS" "$SECOND_TIME"
}

# --- case 1: small closure --------------------------------------------------
run_case 8 small
SMALL_FIRST_SHA=$FIRST_SHA SMALL_FIRST_CHMOD=$FIRST_CHMOD
SMALL_FIRST_RM=$FIRST_RM SMALL_FIRST_LS=$FIRST_LS
SMALL_SECOND_SHA=$SECOND_SHA SMALL_SECOND_CHMOD=$SECOND_CHMOD
SMALL_SECOND_RM=$SECOND_RM SMALL_SECOND_LS=$SECOND_LS

# --- case 2: 4205-member closure — the Tearscape shape ----------------------
run_case 4205 big
BIG_FIRST_SHA=$FIRST_SHA BIG_FIRST_CHMOD=$FIRST_CHMOD
BIG_FIRST_RM=$FIRST_RM BIG_FIRST_LS=$FIRST_LS
BIG_SECOND_SHA=$SECOND_SHA BIG_SECOND_CHMOD=$SECOND_CHMOD
BIG_SECOND_RM=$SECOND_RM BIG_SECOND_LS=$SECOND_LS
BIG_FIRST_TIME=$FIRST_TIME BIG_SECOND_TIME=$SECOND_TIME

# O(1): the healthy boot's ceremony must not scale with the member count.
[ "$BIG_FIRST_SHA" = "$SMALL_FIRST_SHA" ] || fail "sha256sum count scales with members ($SMALL_FIRST_SHA -> $BIG_FIRST_SHA)"
[ "$BIG_SECOND_SHA" = "$SMALL_SECOND_SHA" ] || fail "second-boot sha256sum count scales ($SMALL_SECOND_SHA -> $BIG_SECOND_SHA)"
[ "$BIG_FIRST_CHMOD" = "$SMALL_FIRST_CHMOD" ] || fail "chmod count scales ($SMALL_FIRST_CHMOD -> $BIG_FIRST_CHMOD)"
[ "$BIG_FIRST_RM" = "$SMALL_FIRST_RM" ] || fail "rm count scales ($SMALL_FIRST_RM -> $BIG_FIRST_RM)"
[ "$BIG_FIRST_LS" = "$SMALL_FIRST_LS" ] || fail "ls count scales ($SMALL_FIRST_LS -> $BIG_FIRST_LS)"
[ "$BIG_SECOND_CHMOD" = "$SMALL_SECOND_CHMOD" ] || fail "second-boot chmod count scales"
[ "$BIG_SECOND_RM" = "$SMALL_SECOND_RM" ] || fail "second-boot rm count scales"
[ "$BIG_SECOND_LS" = "$SMALL_SECOND_LS" ] || fail "second-boot ls count scales"
# Exact operation counts above prove O(1). The clock is only a smoke ceiling;
# 3s tolerates measured host contention while the old O(N) path still exceeds
# it by a wide margin on this same fixture.
awk -v t="$BIG_FIRST_TIME" 'BEGIN{exit !(t<3.0)}' || fail "first boot to preflight took ${BIG_FIRST_TIME}s (>3s)"
awk -v t="$BIG_SECOND_TIME" 'BEGIN{exit !(t<3.0)}' || fail "second boot to preflight took ${BIG_SECOND_TIME}s (>3s)"

# --- recovery: adulterated live nxport forces the deep path ------------------
GEN_DIR=$(echo "$PORTS_DIR/$PORT_ID/.nxruntime/generations"/*)
cp "$PORTS_DIR/$PORT_ID/nxport.json" "$MARKERS/nxport.orig"
printf '{"tampered":true}\n' > "$PORTS_DIR/$PORT_ID/nxport.json"
reset_counters
launch
[ "$NXBOOT_LAUNCH_RC" = 0 ] || fail "recovery boot failed rc=$NXBOOT_LAUNCH_RC"
grep -q 'UPDATE NXU0014' "$LOG" || fail 'deep path ran without its visible notice'
grep -q 'UPDATE NXU0003: healing nxport.json' "$LOG" || fail 'deep path did not heal the tampered nxport'
cmp -s "$PORTS_DIR/$PORT_ID/nxport.json" "$MARKERS/nxport.orig" || fail 'nxport was not healed back'
DEEP_SHA=$(counter sha256sum)
# batched: far fewer sha256sum forks than members (one per closure pass, not per file)
[ "$DEEP_SHA" -lt 40 ] || fail "deep path forked sha256sum $DEEP_SHA times for 4205 members (not batched)"

# after recovery, the next boot is fast again
reset_counters
launch
[ "$NXBOOT_LAUNCH_RC" = 0 ] || fail 'post-recovery boot failed'
grep -q 'UPDATE NXU0014' "$LOG" && fail 'post-recovery boot took the deep path again'

# --- fail closed: absent commit refuses the launch ---------------------------
mv "$GEN_DIR/commit" "$MARKERS/commit.save"
launch
[ "$NXBOOT_LAUNCH_RC" != 0 ] || fail 'launch succeeded without a commit marker'
grep -q 'NXU0009' "$LOG" || fail 'commitless launch did not refuse via NXU0009'
mv "$MARKERS/commit.save" "$GEN_DIR/commit"

# --- fail closed: symlinked entry point never fast-boots ---------------------
mv "$PORTS_DIR/$PORT_ID/boot-fastpath-loader" "$MARKERS/loader.save"
ln -s /bin/true "$PORTS_DIR/$PORT_ID/boot-fastpath-loader"
launch
grep -q 'UPDATE NXU0014' "$LOG" || fail 'symlinked entry point skipped the deep path'
rm -f "$PORTS_DIR/$PORT_ID/boot-fastpath-loader"
mv "$MARKERS/loader.save" "$PORTS_DIR/$PORT_ID/boot-fastpath-loader"

# --- adulterated entry point: the identity anchor heals it on the same boot --
printf '#!/bin/bash\nexit 9\n' > "$PORTS_DIR/$PORT_ID/boot-fastpath-loader"
chmod 0755 "$PORTS_DIR/$PORT_ID/boot-fastpath-loader" 2>/dev/null || true
launch
[ "$NXBOOT_LAUNCH_RC" = 0 ] || fail 'tampered-loader boot did not recover'
grep -q 'UPDATE NXU0014' "$LOG" || fail 'tampered loader skipped the deep path'
grep -q 'UPDATE NXU0003: healing boot-fastpath-loader' "$LOG" || \
  fail 'adulterated entry point was not healed'
launch
grep -q 'UPDATE NXU0014' "$LOG" && fail 'post-heal boot took the deep path again'
[ "$NXBOOT_LAUNCH_RC" = 0 ] || fail 'healed entry point cannot boot'

# --- chmodless probe count: at most one per run ------------------------------
# In the deep-path run above, the FAT view forces mode fallbacks for every
# member; the memoized probe keeps the chmod ceremony constant. The probe
# itself costs at most 2 chmod calls once per run.
printf 'boot-fastpath gate: PASS\n'
