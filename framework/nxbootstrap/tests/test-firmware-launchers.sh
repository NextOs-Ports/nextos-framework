#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Eight-profile coverage is cumulative: this legacy v1 launcher gate is kept
# alongside the separate v2 eight-profile/14-case matrix in isolated-suite.sh.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$TEST_DIR/../../.." && pwd -P)
PROFILES=$REPO_ROOT/framework/tests/firmware-profiles-v1.json
GENERATOR=$REPO_ROOT/framework/nxbootstrap/tools/generate-port.py
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/firmware-launchers.XXXXXX")
cleanup() {
  local status=$?
  trap - EXIT
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/firmware-launchers.??????)
      find "$TEST_ROOT" -depth -delete || status=1
      ;;
    *)
      printf 'firmware launcher gate: unsafe cleanup root %s\n' \
        "$TEST_ROOT" >&2
      status=1
      ;;
  esac
  exit "$status"
}
trap cleanup EXIT

fail() {
  printf 'firmware launcher gate failed: %s\n' "$*" >&2
  exit 1
}

run_profile() {
  local profile_id=$1 architecture=$2 cfw_name=$3 port32=$4 suffix=$5
  local profile_root=$TEST_ROOT/$profile_id
  local xdg_root=$profile_root/xdg
  local pm_root=$xdg_root/PortMaster
  local launcher_root=$profile_root/scripts
  local data_root=$profile_root/data
  local runtime_root=$profile_root/runtime
  local markers=$profile_root/markers
  local generated=$profile_root/generated
  local manifest=$profile_root/nxport.json
  local launcher loader log status=0

  mkdir -p "$pm_root" "$launcher_root" "$data_root/ports" \
    "$runtime_root" "$markers"
  mkdir -p "$pm_root/$suffix"
  chmod 0700 "$runtime_root"
  cat > "$pm_root/control.txt" <<CONTROL
directory="${data_root#/}"
controlfolder="$pm_root"
CFW_NAME="$cfw_name"
ESUDO=""
CUR_TTY=/dev/null
sdl_controllerconfig="fixture-guid,$profile_id Pad,a:b0"
get_controls() {
  ANALOGSTICKS=2
  printf 'controls\n' >> "$markers/get-controls"
}
pm_platform_helper() {
  printf '%s\n' "\$1" >> "$markers/platform-helper"
}
pm_finish() {
  printf 'finish\n' >> "$markers/pm-finish"
}
CONTROL

  cat > "$manifest" <<JSON
{
  "schema_version": 2,
  "id": "firmware-fixture",
  "title": "Firmware Contract Fixture",
  "launcher_name": "Firmware Contract Fixture.sh",
  "architecture": "$architecture",
  "executable": "fixture-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.2.17"},
  "required_files": ["fixture-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
  python3 -B "$GENERATOR" "$manifest" --output "$generated" >/dev/null ||
    fail "$profile_id generator failure"
  launcher=$generated/Firmware\ Contract\ Fixture.sh
  bash -n "$launcher" || fail "$profile_id launcher syntax"
  if [[ $port32 == true ]]; then
    grep -Fqx 'PORT_32BIT="Y"' "$launcher" ||
      fail "$profile_id lacks the ARMHF scanner literal"
  elif grep -Fq PORT_32BIT "$launcher"; then
    fail "$profile_id gained an ARMHF scanner literal"
  fi
  grep -Fq '"$controlfolder/'"$suffix"'"' "$launcher" ||
    fail "$profile_id lacks the expected architecture library suffix"

  cp "$launcher" "$launcher_root/"
  mkdir -p "$data_root/ports/firmware-fixture"
  cp "$REPO_ROOT/framework/nxbootstrap/tests/splash-stub.sh" \
    "$data_root/ports/firmware-fixture/nxsplash-nextos"
  chmod 0755 "$data_root/ports/firmware-fixture/nxsplash-nextos"
  loader=$data_root/ports/firmware-fixture/fixture-loader
  cat > "$loader" <<LOADER
#!/bin/bash
{
  printf 'portmaster=%s\n' "\${NXCOMPAT_PORTMASTER_DIR-}"
  printf 'mapping=%s\n' "\${SDL_GAMECONTROLLERCONFIG-}"
  printf 'sticks=%s\n' "\${NXINPUT_ANALOG_STICKS_HINT-}"
  printf 'port32=%s\n' "\${PORT_32BIT-}"
  printf 'library_path=%s\n' "\${LD_LIBRARY_PATH-}"
} > "$markers/child-env"
exit 23
LOADER
  chmod 0755 "$loader"

  # No stat binary is present in the synthetic first PATH entry. A sentinel
  # catches accidental fallback to the host command without pretending that
  # this reproduces BusyBox or a firmware rootfs.
  mkdir -p "$profile_root/no-stat"
  cat > "$profile_root/no-stat/stat" <<STAT
#!/bin/bash
: > "$markers/stat-called"
exit 127
STAT
  chmod 0755 "$profile_root/no-stat/stat"
  env -i PATH="$profile_root/no-stat:$PATH" HOME="$profile_root" \
    TMPDIR="${TMPDIR:-/tmp}" XDG_DATA_HOME="$xdg_root" \
    XDG_RUNTIME_DIR="$runtime_root" \
    bash "$launcher_root/Firmware Contract Fixture.sh" \
    </dev/null >/dev/null 2>&1 || status=$?
  [[ $status == 23 ]] || fail "$profile_id returned $status instead of 23"
  [[ ! -e $markers/stat-called && -s $markers/child-env ]] ||
    fail "$profile_id called stat or did not launch the child"
  [[ $(wc -l < "$markers/get-controls") == 1 ]] ||
    fail "$profile_id get_controls count"
  [[ $(wc -l < "$markers/platform-helper") == 1 ]] ||
    fail "$profile_id pm_platform_helper count"
  [[ $(wc -l < "$markers/pm-finish") == 1 ]] ||
    fail "$profile_id pm_finish count"
  grep -Fqx "portmaster=$pm_root" "$markers/child-env" ||
    fail "$profile_id lost the active PortMaster root"
  grep -Fqx "mapping=fixture-guid,$profile_id Pad,a:b0" \
    "$markers/child-env" || fail "$profile_id lost controller mapping"
  grep -Fqx 'sticks=2' "$markers/child-env" ||
    fail "$profile_id lost the validated stick hint"
  if [[ $port32 == true ]]; then
    grep -Fqx 'port32=Y' "$markers/child-env" ||
      fail "$profile_id did not publish PORT_32BIT"
  else
    grep -Fqx 'port32=' "$markers/child-env" ||
      fail "$profile_id unexpectedly published PORT_32BIT"
  fi
  grep -Fq "$pm_root/$suffix" "$markers/child-env" ||
    fail "$profile_id lost PortMaster library precedence"
  log=$data_root/ports/firmware-fixture/log.txt
  grep -Fq "cfw=$cfw_name" "$log" || fail "$profile_id log lacks CFW fact"
  grep -Fq 'end (status 23)' "$log" ||
    fail "$profile_id log lacks the child status"
}

profile_count=0
while IFS=$'\t' read -r profile_id architecture cfw_name port32 suffix; do
  run_profile "$profile_id" "$architecture" "$cfw_name" "$port32" "$suffix"
  profile_count=$((profile_count + 1))
done < <(python3 -B - "$PROFILES" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    document = json.load(stream)
for profile in document["profiles"]:
    print("\t".join((
        profile["id"], profile["launcher_architecture"],
        profile["cfw_name"], str(profile["port_32bit"]).lower(),
        profile["arch_library_suffix"],
    )))
PY
)

printf 'firmware launcher contract gate passed: profiles=%s ' "$profile_count"
printf 'result=profile-contract-pass no_stat=%s split_roots=%s ' \
  "$profile_count" "$profile_count"
printf 'hardware_ran=0 device_access=0 firmware_images_used=0\n'
