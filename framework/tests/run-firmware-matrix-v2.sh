#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Execute generated launchers in disposable synthetic firmware roots. This is
# a process test and is allowed only inside nxbootstrap's sealed namespace.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$TEST_DIR/../.." && pwd -P)
BOOTSTRAP_TEST_DIR=$REPO_ROOT/framework/nxbootstrap/tests
PROFILES=$TEST_DIR/firmware-profiles-v2.json
GENERATOR=$REPO_ROOT/framework/nxbootstrap/tools/generate-port.py
VALIDATOR=$TEST_DIR/test_firmware_matrix_v2.py
BOOTSTRAP_VERSION=$(<"$REPO_ROOT/framework/nxbootstrap/VERSION")
# shellcheck source=../nxbootstrap/tests/private-pid-namespace.sh
source "$BOOTSTRAP_TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

case $# in
  0) RECEIPT_OUTPUT= ;;
  2)
    [[ $1 == --receipt && $2 == /* ]] || {
      printf 'usage: %s [--receipt ABSOLUTE_JSON]\n' "${0##*/}" >&2
      exit 2
    }
    RECEIPT_OUTPUT=$2
    ;;
  *)
    printf 'usage: %s [--receipt ABSOLUTE_JSON]\n' "${0##*/}" >&2
    exit 2
    ;;
esac

command -v bwrap >/dev/null 2>&1 || {
  printf 'firmware matrix v2: SKIP (bubblewrap is unavailable)\n' >&2
  exit 77
}

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/firmware-matrix-v2.XXXXXX")
cleanup() {
  local status=$?
  trap - EXIT
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/firmware-matrix-v2.??????)
      find "$TEST_ROOT" -depth -delete || status=1
      ;;
    *)
      printf 'firmware matrix v2: unsafe cleanup root %s\n' "$TEST_ROOT" >&2
      status=1
      ;;
  esac
  exit "$status"
}
trap cleanup EXIT

fail() {
  printf 'firmware matrix v2 failed: %s\n' "$*" >&2
  exit 1
}

TOOLBOX=$TEST_ROOT/no-stat-bin
mkdir -p "$TOOLBOX"
for tool in chmod dirname flock id ls mkdir mv readlink sleep; do
  target=$(command -v "$tool")
  [[ $target == /* && -x $target ]] || fail "missing host tool $tool"
  ln -s "$target" "$TOOLBOX/$tool"
done
[[ ! -e $TOOLBOX/stat && ! -L $TOOLBOX/stat ]] || fail 'stat entered toolbox'

# E5: casos de campo precisam do preflight real (grep/head/basename) -- ainda
# sem stat, que continua proibido.
TOOLBOX_FIELD=$TEST_ROOT/no-stat-bin-field
cp -a "$TOOLBOX" "$TOOLBOX_FIELD"
for tool in grep head basename cat; do
  target=$(command -v "$tool")
  [[ $target == /* && -x $target ]] || fail "missing host tool $tool"
  ln -s "$target" "$TOOLBOX_FIELD/$tool"
done
[[ ! -e $TOOLBOX_FIELD/stat ]] || fail 'stat entered field toolbox'

RECORDS=$TEST_ROOT/records.tsv
: > "$RECORDS"

add_bwrap_dirs() {
  local path=$1 current= part
  local -n arguments=$2
  local -n observed=$3
  IFS=/ read -r -a parts <<< "${path#/}"
  for part in "${parts[@]}"; do
    [[ -n $part ]] || continue
    current=$current/$part
    if [[ -z ${observed[$current]+x} ]]; then
      arguments+=(--dir "$current")
      observed[$current]=1
    fi
  done
}

run_sandbox() {
  local fixture_root=$1 data_source=$2 pm_source=$3
  local data_root=$4 pm_root=$5 launcher=$6 mode=$7
  local profile_id=$8 architecture=$9
  local -a args=(
    --unshare-user --unshare-pid --unshare-net --unshare-ipc --unshare-uts
    --die-with-parent --tmpfs /
  )
  local path relative_pm
  declare -A dirs=()

  for path in /usr /bin /lib /lib64; do
    [[ -e $path ]] && args+=(--ro-bind "$path" "$path")
  done
  args+=(--proc /proc --dev /dev --tmpfs /tmp)
  add_bwrap_dirs /fixture args dirs
  add_bwrap_dirs "$data_root" args dirs
  add_bwrap_dirs "$pm_root" args dirs
  if [[ $profile_id == darkosre ]]; then
    add_bwrap_dirs /boot args dirs
    add_bwrap_dirs /opt/system/Advanced args dirs
  fi
  args+=(--bind "$fixture_root" /fixture)
  args+=(--bind "$data_source" "$data_root")
  if [[ $pm_root == "$data_root"/* ]]; then
    relative_pm=${pm_root#"$data_root"/}
    mkdir -p "$data_source/$relative_pm"
  fi
  args+=(--bind "$pm_source" "$pm_root")
  if [[ $profile_id == darkosre ]]; then
    args+=(
      --ro-bind "$fixture_root/darkos/arkos4clone-uboot.dtb"
        /boot/arkos4clone-uboot.dtb
      --ro-bind "$fixture_root/darkos/Backup dArkOS Settings.sh"
        "/opt/system/Advanced/Backup dArkOS Settings.sh"
    )
  fi
  # E5: fixture REAL do device (AmberELEC 20230203): o alsoft.conf da CFW com
  # drivers=alsa, exatamente onde o OpenAL embutido do guest o leria.
  if [[ $profile_id == amberelec ]]; then
    add_bwrap_dirs /etc/openal args dirs
    args+=(--ro-bind \
      "$REPO_ROOT/framework/tests/fixtures/amberelec/etc/openal/alsoft.conf" \
      /etc/openal/alsoft.conf)
  fi
  # E5 (spruce synthetic route): the real off-path ARMHF loader is described
  # and verified by device-environments/spruce. This small sandbox uses only a
  # routing stub because the main matrix must not store firmware binaries.
  if [[ -n ${NXMATRIX_ALT_ARMHF_LOADER-} ]]; then
    add_bwrap_dirs /mnt/SDCARD/spruce/flip args dirs
    args+=(--ro-bind "$NXMATRIX_ALT_ARMHF_LOADER" \
      /mnt/SDCARD/spruce/flip/ld-linux-armhf.so.3)
  fi

  bwrap "${args[@]}" /usr/bin/env -i \
    PATH=/fixture/no-stat-bin HOME=/fixture/home TMPDIR=/tmp \
    XDG_DATA_HOME=/fixture/xdg-unrelated \
    XDG_RUNTIME_DIR=/fixture/runtime \
    NXMATRIX_MODE="$mode" NXMATRIX_PROFILE="$profile_id" \
    NXMATRIX_ARCH="$architecture" \
    /bin/bash -c \
      'command -v stat >/dev/null 2>&1 && exit 91; exec /bin/bash "$1"' \
    nxmatrix "$launcher"
}

run_case() {
  local profile_id=$1 architecture=$2 cfw_name=$3 pm_root=$4
  local data_root=$5 suffix=$6 port32=$7
  local case_root=$TEST_ROOT/cases/$profile_id/$architecture
  local fixture_root=$case_root/fixture
  local data_source=$case_root/data
  local pm_source=$case_root/portmaster
  local generated=$case_root/generated
  local game_dir=$data_source/firmware-matrix-fixture
  local early_dir=$data_source/.nxmatrix-early-$architecture
  local manifest=$case_root/nxport.json
  local directory=${data_root%/ports}
  local launcher="$data_root/Firmware Matrix Fixture.sh"
  local early_launcher="$data_root/.nxmatrix-early-$architecture/Firmware Matrix Fixture.sh"
  local status=0 early_status=0 early_log mode

  mkdir -p "$fixture_root/home/.config" "$fixture_root/runtime" \
    "$fixture_root/markers" "$fixture_root/xdg-unrelated" \
    "$fixture_root/darkos" "$data_source" "$pm_source/$suffix" \
    "$game_dir" "$early_dir"
  chmod 0700 "$fixture_root/runtime"
  cp -a "$TOOLBOX" "$fixture_root/no-stat-bin"
  printf '%s\n' "$cfw_name" > "$fixture_root/home/.config/.OS"
  : > "$fixture_root/darkos/arkos4clone-uboot.dtb"
  : > "$fixture_root/darkos/Backup dArkOS Settings.sh"

  cat > "$pm_source/control.txt" <<CONTROL
if [ "\${NXMATRIX_MODE-}" = early ]; then
  directory="fixture/missing-root"
else
  directory="${directory#/}"
fi
controlfolder="$pm_root"
ESUDO=""
CUR_TTY=/dev/null
sdl_controllerconfig="fixture-guid,$profile_id Pad,a:b0"
get_controls() {
  ANALOGSTICKS=2
  printf 'get-controls\n' >> "/fixture/markers/get-controls-\${NXMATRIX_MODE}-\${NXMATRIX_ARCH}"
}
pm_platform_helper() {
  printf '%s\n' "\$1" >> "/fixture/markers/platform-helper-\${NXMATRIX_MODE}-\${NXMATRIX_ARCH}"
}
pm_finish() {
  printf 'finish\n' >> "/fixture/markers/pm-finish-\${NXMATRIX_MODE}-\${NXMATRIX_ARCH}"
}
CONTROL
  if [[ $profile_id != darkosre ]]; then
    printf 'CFW_NAME=%q\n' "$cfw_name" >> "$pm_source/control.txt"
  fi

  cat > "$manifest" <<JSON
{
  "schema_version": 2,
  "id": "firmware-matrix-fixture",
  "title": "Firmware Matrix Fixture",
  "launcher_name": "Firmware Matrix Fixture.sh",
  "architecture": "$architecture",
  "executable": "firmware-matrix-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["firmware-matrix-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
  python3 -B "$GENERATOR" "$manifest" --output "$generated" >/dev/null ||
    fail "$profile_id/$architecture generator failure"
  if grep -Fq '"version": "1.2.8"' "$manifest" ||
      grep -Fq 'nxbootstrap 0.6.13' \
        "$generated/Firmware Matrix Fixture.sh"; then
    fail "$profile_id/$architecture retained a previous framework token"
  fi
  bash -n "$generated/Firmware Matrix Fixture.sh" ||
    fail "$profile_id/$architecture launcher syntax"
  if [[ $port32 == true ]]; then
    grep -Fqx 'PORT_32BIT="Y"' "$generated/Firmware Matrix Fixture.sh" ||
      fail "$profile_id/$architecture lacks PORT_32BIT literal"
  elif grep -Fq 'PORT_32BIT=' "$generated/Firmware Matrix Fixture.sh"; then
    fail "$profile_id/$architecture gained PORT_32BIT"
  fi
  grep -Fq '"$controlfolder/'"$suffix"'"' \
    "$generated/Firmware Matrix Fixture.sh" ||
    fail "$profile_id/$architecture library route"

  cp "$generated/Firmware Matrix Fixture.sh" "$data_source/"
  cp "$generated/Firmware Matrix Fixture.sh" "$early_dir/"
  cp "$generated/firmware-matrix-fixture/nxsplash-nextos" \
    "$game_dir/nxsplash-generated"
  cmp -s "$game_dir/nxsplash-generated" \
    "$REPO_ROOT/framework/nxsplash/release/$architecture/nxsplash-nextos" ||
    fail "$profile_id/$architecture selected a cross-ABI splash"
  cp "$BOOTSTRAP_TEST_DIR/splash-stub.sh" "$game_dir/nxsplash-nextos"
  chmod 0755 "$game_dir/nxsplash-nextos"
  cat > "$game_dir/firmware-matrix-loader" <<'LOADER'
#!/bin/bash
{
  printf 'portmaster=%s\n' "${NXCOMPAT_PORTMASTER_DIR-}"
  printf 'mapping=%s\n' "${SDL_GAMECONTROLLERCONFIG-}"
  printf 'sticks=%s\n' "${NXINPUT_ANALOG_STICKS_HINT-}"
  printf 'port32=%s\n' "${PORT_32BIT-}"
  printf 'library_path=%s\n' "${LD_LIBRARY_PATH-}"
  printf 'alsoft=%s\n' "${ALSOFT_DRIVERS-}"
} > "/fixture/markers/child-env-${NXMATRIX_ARCH}"
exit 37
LOADER
  chmod 0755 "$game_dir/firmware-matrix-loader"

  status=0
  run_sandbox "$fixture_root" "$data_source" "$pm_source" \
    "$data_root" "$pm_root" "$launcher" normal \
    "$profile_id" "$architecture" || status=$?
  [[ $status == 37 ]] ||
    fail "$profile_id/$architecture returned $status instead of 37"
  [[ $(wc -l < "$fixture_root/markers/pm-finish-normal-$architecture") == 1 ]] ||
    fail "$profile_id/$architecture normal pm_finish count"
  [[ $(wc -l < "$fixture_root/markers/get-controls-normal-$architecture") == 1 ]] ||
    fail "$profile_id/$architecture get_controls count"
  [[ $(wc -l < "$fixture_root/markers/platform-helper-normal-$architecture") == 1 ]] ||
    fail "$profile_id/$architecture platform helper count"
  grep -Fqx "portmaster=$pm_root" \
    "$fixture_root/markers/child-env-$architecture" ||
    fail "$profile_id/$architecture lost PortMaster root"
  grep -Fqx "mapping=fixture-guid,$profile_id Pad,a:b0" \
    "$fixture_root/markers/child-env-$architecture" ||
    fail "$profile_id/$architecture lost mapping"
  grep -Fqx 'sticks=2' "$fixture_root/markers/child-env-$architecture" ||
    fail "$profile_id/$architecture lost stick hint"
  if [[ $port32 == true ]]; then
    grep -Fqx 'port32=Y' "$fixture_root/markers/child-env-$architecture" ||
      fail "$profile_id/$architecture did not export PORT_32BIT"
  else
    grep -Fqx 'port32=' "$fixture_root/markers/child-env-$architecture" ||
      fail "$profile_id/$architecture unexpectedly exported PORT_32BIT"
  fi
  grep -Fq "$pm_root/$suffix" \
    "$fixture_root/markers/child-env-$architecture" ||
    fail "$profile_id/$architecture lost library precedence"
  if [[ $profile_id == amberelec ]]; then
    grep -Fqx 'alsoft=' "$fixture_root/markers/child-env-$architecture" ||
      fail "amberelec/$architecture leaked ALSOFT_DRIVERS without capability"
  fi
  grep -Fq "cfw=$cfw_name" "$game_dir/log.txt" ||
    fail "$profile_id/$architecture log lacks safe CFW"
  grep -Fq 'end (status 37)' "$game_dir/log.txt" ||
    fail "$profile_id/$architecture log lacks child status"

  early_status=0
  run_sandbox "$fixture_root" "$data_source" "$pm_source" \
    "$data_root" "$pm_root" "$early_launcher" early \
    "$profile_id" "$architecture" >/dev/null 2>&1 || early_status=$?
  [[ $early_status == 1 ]] ||
    fail "$profile_id/$architecture early failure returned $early_status"
  shopt -s nullglob
  early_logs=("$early_dir"/firmware-matrix-fixture-launcher-error.*.log)
  shopt -u nullglob
  [[ ${#early_logs[@]} == 1 ]] ||
    fail "$profile_id/$architecture early-log count ${#early_logs[@]}"
  early_log=${early_logs[0]}
  mode=$(python3 -B - "$early_log" <<'PY'
import os
import stat
import sys
print("%04o" % stat.S_IMODE(os.lstat(sys.argv[1]).st_mode))
PY
)
  [[ $mode == 0600 ]] || fail "$profile_id/$architecture early-log mode $mode"
  for token in "nxbootstrap $BOOTSTRAP_VERSION" 'status=1 pid=' 'launcher=' \
    'game_dir=' "cfw=$cfw_name"; do
    grep -Fq "$token" "$early_log" ||
      fail "$profile_id/$architecture early log lacks $token"
  done
  [[ $(wc -l < "$fixture_root/markers/pm-finish-early-$architecture") == 1 ]] ||
    fail "$profile_id/$architecture early pm_finish count"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$profile_id" "$architecture" "$cfw_name" "$pm_root" "$data_root" \
    >> "$RECORDS"
}

while IFS=$'\t' read -r profile_id architecture cfw_name pm_root \
    data_root suffix port32; do
  run_case "$profile_id" "$architecture" "$cfw_name" "$pm_root" \
    "$data_root" "$suffix" "$port32"
done < <(python3 -B - "$PROFILES" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    document = json.load(stream)
for profile in document["profiles"]:
    for case in profile["arch_cases"]:
        print("\t".join((
            profile["id"], case["architecture"], profile["firmware_names"][0],
            profile["portmaster"]["primary_root"],
            profile["portmaster"]["data_root"], case["library_suffix"],
            str(case["port_32bit"]).lower(),
        )))
PY
)

# ---------------------------------------------------------------------------
# E5: casos de bug de CAMPO (test-first). Cada caso reproduz o ambiente real
# que causou o bug e prova que o fix da classe responde -- e cada um tem o
# espelho invertido (sem o fix, o caso reprovaria).

field_case_embedded_openal() {
  # Classe Tightrope/AmberELEC: alsoft.conf da CFW (drivers=alsa) + capability
  # audio.embedded-openal => o launcher TEM que cravar ALSOFT_DRIVERS=opensl.
  # O espelho invertido e o assert 'alsoft=' vazio no caso padrao do amberelec.
  local case_root=$TEST_ROOT/cases/amberelec/field-embedded-openal
  local fixture_root=$case_root/fixture
  local data_source=$case_root/data
  local pm_source=$case_root/portmaster
  local generated=$case_root/generated
  local data_root=/roms/ports pm_root=/roms/ports/PortMaster
  local game_dir=$data_source/firmware-matrix-fixture
  local launcher="$data_root/Firmware Matrix Fixture.sh"
  local status=0

  mkdir -p "$fixture_root/home/.config" "$fixture_root/runtime" \
    "$fixture_root/markers" "$fixture_root/xdg-unrelated" \
    "$fixture_root/darkos" "$data_source" "$pm_source/libs.aarch64" "$game_dir"
  chmod 0700 "$fixture_root/runtime"
  cp -a "$TOOLBOX" "$fixture_root/no-stat-bin"
  printf 'AmberELEC\n' > "$fixture_root/home/.config/.OS"
  cp "$TEST_ROOT/cases/amberelec/aarch64/portmaster/control.txt" \
    "$pm_source/control.txt"

  cat > "$case_root/nxport.json" <<JSON
{
  "schema_version": 2,
  "id": "firmware-matrix-fixture",
  "title": "Firmware Matrix Fixture",
  "launcher_name": "Firmware Matrix Fixture.sh",
  "architecture": "aarch64",
  "executable": "firmware-matrix-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["firmware-matrix-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": ["audio.embedded-openal"],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
  python3 -B "$GENERATOR" "$case_root/nxport.json" --output "$generated" \
    >/dev/null || fail "field embedded-openal generator failure"
  cp "$generated/Firmware Matrix Fixture.sh" "$data_source/"
  cp "$BOOTSTRAP_TEST_DIR/splash-stub.sh" "$game_dir/nxsplash-nextos"
  chmod 0755 "$game_dir/nxsplash-nextos"
  cat > "$game_dir/firmware-matrix-loader" <<'LOADER'
#!/bin/bash
{
  printf 'alsoft=%s\n' "${ALSOFT_DRIVERS-}"
  printf 'hostconf=%s\n' "$(cat /etc/openal/alsoft.conf 2>/dev/null |
    grep -c 'drivers=alsa' || true)"
} > "/fixture/markers/field-openal"
exit 37
LOADER
  chmod 0755 "$game_dir/firmware-matrix-loader"
  # o dump do guest usa grep/cat: toolbox de campo
  cp -a "$TOOLBOX_FIELD/." "$fixture_root/no-stat-bin/"
  rm -f "$fixture_root/no-stat-bin/stat"

  run_sandbox "$fixture_root" "$data_source" "$pm_source" \
    "$data_root" "$pm_root" "$launcher" normal amberelec aarch64 \
    > "$case_root/stdout.log" 2>&1 || status=$?
  [[ $status == 37 ]] ||
    fail "field embedded-openal returned $status instead of 37"
  grep -Fqx 'alsoft=opensl' "$fixture_root/markers/field-openal" ||
    fail 'field embedded-openal: shield did not pin ALSOFT_DRIVERS=opensl'
  grep -Fqx 'hostconf=1' "$fixture_root/markers/field-openal" ||
    fail 'field embedded-openal: real alsoft.conf fixture is not in place'
  grep -Fq 'AUDIO GUEST: ALSOFT_DRIVERS=opensl pinned' \
    "$game_dir/log.txt" ||
    fail 'field embedded-openal: launcher log lacks the AUDIO GUEST line'
  grep -Fq 'ENV RECEIPT:' "$game_dir/log.txt" ||
    fail 'field embedded-openal: launcher log lacks the ENV RECEIPT'
  grep -Fq 'alsoft_drivers=opensl' "$game_dir/log.txt" ||
    fail 'field embedded-openal: ENV RECEIPT does not carry opensl'
  printf 'field-case embedded-openal-shield: PASS\n'
}

field_case_armhf_preflight() {
  # Classe Titan Souls/spruce: o caminho ARMHF padrao esta ausente, mas a CFW
  # monta um loader e closures ARMHF fora dele.
  # Caso A (loader ausente em TODO caminho): mensagem CLARA + status 1 (nunca
  # o 127 seco). Caso B: o stub prova somente o roteamento do launcher. O
  # loader, os ELFs e a SDL reais sao provados no ambiente PC opcional.
  local variant=$1 case_root fixture_root data_source pm_source generated
  local data_root=/mnt/sdcard/Roms/PORTS64
  local pm_root=/mnt/sdcard/Roms/.portmaster/PortMaster
  local game_dir launcher status=0
  case_root=$TEST_ROOT/cases/spruce/field-armhf-$variant
  fixture_root=$case_root/fixture
  data_source=$case_root/data
  pm_source=$case_root/portmaster
  generated=$case_root/generated
  game_dir=$data_source/firmware-matrix-fixture
  launcher="$data_root/Firmware Matrix Fixture.sh"

  mkdir -p "$fixture_root/home/.config" "$fixture_root/runtime" \
    "$fixture_root/markers" "$fixture_root/xdg-unrelated" \
    "$fixture_root/darkos" "$data_source" "$pm_source/libs.armhf" "$game_dir"
  chmod 0700 "$fixture_root/runtime"
  cp -a "$TOOLBOX_FIELD" "$fixture_root/no-stat-bin"
  printf 'spruce\n' > "$fixture_root/home/.config/.OS"
  cp "$TEST_ROOT/cases/spruce/aarch64/portmaster/control.txt" \
    "$pm_source/control.txt"

  cat > "$case_root/nxport.json" <<JSON
{
  "schema_version": 2,
  "id": "firmware-matrix-fixture",
  "title": "Firmware Matrix Fixture",
  "launcher_name": "Firmware Matrix Fixture.sh",
  "architecture": "armv7",
  "executable": "firmware-matrix-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["firmware-matrix-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
  python3 -B "$GENERATOR" "$case_root/nxport.json" --output "$generated" \
    >/dev/null || fail "field armhf generator failure"
  cp "$generated/Firmware Matrix Fixture.sh" "$data_source/"
  # splash TAMBEM e ELF ARMHF real no alt-loader (caso de campo: direto = 127)
  if [[ $variant == alt-loader ]]; then
    cp "$REPO_ROOT/framework/nxsplash/release/armv7/nxsplash-nextos" \
      "$game_dir/nxsplash-nextos"
  else
    cp "$BOOTSTRAP_TEST_DIR/splash-stub.sh" "$game_dir/nxsplash-nextos"
  fi
  chmod 0755 "$game_dir/nxsplash-nextos"
  # executavel = ELF ARMHF REAL (nxsplash armv7): o preflight le o .interp dele
  cp "$REPO_ROOT/framework/nxsplash/release/armv7/nxsplash-nextos" \
    "$game_dir/firmware-matrix-loader"
  chmod 0755 "$game_dir/firmware-matrix-loader"

  if [[ $variant == alt-loader ]]; then
    cat > "$case_root/fake-armhf-loader" <<'FAKE'
#!/bin/bash
printf 'fake-loader argv=%s exe_env=%s\n' "$*" "${NXBOOTSTRAP_EXE-}" \
  >> /fixture/markers/field-armhf-alt
case "$3" in
  *nxsplash*) exit 0 ;;
  *) exit 37 ;;
esac
FAKE
    chmod 0755 "$case_root/fake-armhf-loader"
    NXMATRIX_ALT_ARMHF_LOADER=$case_root/fake-armhf-loader \
      run_sandbox "$fixture_root" "$data_source" "$pm_source" \
      "$data_root" "$pm_root" "$launcher" normal spruce armv7 \
      > "$case_root/stdout.log" 2>&1 || status=$?
    [[ $status == 37 ]] ||
      fail "field armhf alt-loader returned $status instead of 37"
    grep -Fq 'NOTE: interpreter /lib/ld-linux-armhf.so.3 is absent' \
      "$game_dir/log.txt" ||
      fail 'field armhf alt-loader: NOTE line missing'
    grep -Fq 'NOTE: nxsplash also runs through the alternate dynamic loader' \
      "$game_dir/log.txt" ||
      { tail -n 14 "$game_dir/log.txt" >&2 || true
        fail 'field armhf alt-loader: splash did not take the alt loader (127 do campo!)'; }
    grep -Fq 'mandatory handoff complete' "$game_dir/log.txt" ||
      fail 'field armhf alt-loader: splash phase did not complete'
    [[ $(grep -c -- '--library-path' "$fixture_root/markers/field-armhf-alt") -ge 2 ]] ||
      fail 'field armhf alt-loader: splash E jogo devem passar pelo alt loader'
    grep -Fq "exe_env=$data_root/firmware-matrix-fixture/firmware-matrix-loader" \
      "$fixture_root/markers/field-armhf-alt" ||
      fail 'field armhf alt-loader: NXBOOTSTRAP_EXE nao aponta pro executavel REAL (classe /proc/self/exe!)'
    grep -Fq '/mnt/SDCARD/Persistent/.32bit_chroot/usr/lib' \
      "$fixture_root/markers/field-armhf-alt" ||
      fail 'field armhf alt-loader: --library-path sem o chroot ARMHF'
    grep -Fq '/mnt/SDCARD/spruce/flip/muOS/usr/lib32' \
      "$fixture_root/markers/field-armhf-alt" ||
      fail 'field armhf alt-loader: --library-path sem o fallback ARMHF usr/lib32'
    printf 'field-case armhf-interp-preflight(alt-loader): PASS\n'
  else
    run_sandbox "$fixture_root" "$data_source" "$pm_source" \
      "$data_root" "$pm_root" "$launcher" normal spruce armv7 \
      > "$case_root/stdout.log" 2>&1 || status=$?
    [[ $status == 1 ]] ||
      fail "field armhf missing-loader returned $status instead of 1"
    [[ $status != 127 ]] || fail 'field armhf regressed to bare 127'
    grep -Fq 'dynamic loader /lib/ld-linux-armhf.so.3 is missing' \
      "$game_dir/log.txt" ||
      fail 'field armhf: clear preflight message missing'
    grep -Fq '32-bit ARM (ARMHF) port' "$game_dir/log.txt" ||
      fail 'field armhf: ARMHF guidance missing'
    printf 'field-case armhf-interp-preflight(missing): PASS\n'
  fi
}

field_case_python_probe() {
  # Classe muOS "bad marshal data": o python3 do firmware nao inicia (cache de
  # bytecode da stdlib corrompida). Variante broken: nem a cache privada salva
  # -> mensagem CLARA + reason 6209 + saida limpa (nunca um erro criptico do
  # extrator). Variante private-cache: a cache privada resolve -> WARN e a
  # instalacao SEGUE (o fix real do campo).
  local variant=$1 case_root fixture_root data_source pm_source generated
  local data_root=/mnt/mmc/ports pm_root=/mnt/mmc/MUOS/PortMaster
  local game_dir launcher status=0
  case_root=$TEST_ROOT/cases/muos/field-python-$variant
  fixture_root=$case_root/fixture
  data_source=$case_root/data
  pm_source=$case_root/portmaster
  generated=$case_root/generated
  game_dir=$data_source/firmware-matrix-fixture
  launcher="$data_root/Firmware Matrix Fixture.sh"

  mkdir -p "$fixture_root/home/.config" "$fixture_root/runtime" \
    "$fixture_root/markers" "$fixture_root/xdg-unrelated" \
    "$fixture_root/darkos" "$data_source" "$pm_source/libs.aarch64" \
    "$game_dir/nxextract"
  chmod 0700 "$fixture_root/runtime"
  cp -a "$TOOLBOX_FIELD" "$fixture_root/no-stat-bin"
  printf 'muOS\n' > "$fixture_root/home/.config/.OS"
  cp "$TEST_ROOT/cases/muos/aarch64/portmaster/control.txt" \
    "$pm_source/control.txt"

  # python3 do "firmware": broken falha SEMPRE a sonda; private-cache so
  # funciona quando o launcher exporta PYTHONPYCACHEPREFIX (o fix do campo).
  if [[ $variant == broken ]]; then
    cat > "$fixture_root/no-stat-bin/python3" <<'PYSTUB'
#!/bin/bash
echo "ValueError: bad marshal data (unknown type code)" >&2
exit 1
PYSTUB
  else
    cat > "$fixture_root/no-stat-bin/python3" <<'PYSTUB'
#!/bin/bash
if [ -n "${PYTHONPYCACHEPREFIX-}" ]; then exit 0; fi
echo "ValueError: bad marshal data (unknown type code)" >&2
exit 1
PYSTUB
  fi
  chmod 0755 "$fixture_root/no-stat-bin/python3"

  cat > "$case_root/nxport.json" <<JSON
{
  "schema_version": 2,
  "id": "firmware-matrix-fixture",
  "title": "Firmware Matrix Fixture",
  "launcher_name": "Firmware Matrix Fixture.sh",
  "architecture": "aarch64",
  "executable": "firmware-matrix-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "yes", "version": "1.2.18"},
  "required_files": ["firmware-matrix-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
  python3 -B "$GENERATOR" "$case_root/nxport.json" --output "$generated" \
    >/dev/null || fail "field python-probe generator failure"
  cp "$generated/Firmware Matrix Fixture.sh" "$data_source/"
  cp "$BOOTSTRAP_TEST_DIR/splash-stub.sh" "$game_dir/nxsplash-nextos"
  for stub in nxextract-ui run-extractor.sh nxextract-runtime-env.sh \
      nxextract.py; do
    printf '#!/bin/bash\nexit 0\n' > "$game_dir/nxextract/$stub"
    chmod 0755 "$game_dir/nxextract/$stub"
  done
  printf '{"schema": 1}\n' > "$game_dir/extractor.json"
  printf '#!/bin/bash\nexit 37\n' > "$game_dir/firmware-matrix-loader"
  chmod 0755 "$game_dir/nxsplash-nextos" "$game_dir/firmware-matrix-loader"

  run_sandbox "$fixture_root" "$data_source" "$pm_source" \
    "$data_root" "$pm_root" "$launcher" normal muos aarch64 \
    > "$case_root/stdout.log" 2>&1 || status=$?
  if [[ $variant == broken ]]; then
    [[ $status == 1 ]] ||
      fail "field python-probe broken returned $status instead of 1"
    grep -Fq "the firmware's python3 cannot start" "$game_dir/log.txt" || {
      tail -n 12 "$game_dir/log.txt" >&2 || true
      tail -n 8 "$case_root/stdout.log" >&2 || true
      fail 'field python-probe broken: clear 6209 message missing'
    }
    grep -Fq '"reason_code":6209' "$game_dir/log.txt" ||
      fail 'field python-probe broken: phase event 6209 missing'
    printf 'field-case python-probe(broken): PASS\n'
  else
    grep -Fq 'WARN: system python bytecode cache is broken' \
      "$game_dir/log.txt" ||
      fail 'field python-probe private-cache: WARN line missing'
    if grep -Fq "the firmware's python3 cannot start" "$game_dir/log.txt"; then
      fail 'field python-probe private-cache: fix path did not engage'
    fi
    printf 'field-case python-probe(private-cache): PASS\n'
  fi
}

field_case_embedded_openal
field_case_armhf_preflight missing
field_case_armhf_preflight alt-loader
field_case_python_probe broken
field_case_python_probe private-cache

RECEIPT=$TEST_ROOT/firmware-matrix-receipt-v2.json
python3 -B - "$PROFILES" "$RECORDS" "$RECEIPT" <<'PY'
import json
import sys

profiles_path, records_path, output_path = sys.argv[1:]
with open(profiles_path, "r", encoding="utf-8") as stream:
    matrix = json.load(stream)
records = []
with open(records_path, "r", encoding="utf-8") as stream:
    for line in stream:
        profile_id, architecture, cfw_name, pm_root, data_root = line.rstrip("\n").split("\t")
        records.append({
            "id": profile_id,
            "architecture": architecture,
            "cfw_name": cfw_name,
            "portmaster_root": pm_root,
            "data_root": data_root,
            "result": "synthetic-contract-pass",
            "checks": {
                "root_and_control": True,
                "cfw_detection": True,
                "path_without_stat": True,
                "normal_status": True,
                "early_log_0600": True,
                "early_log_fields": True,
                "early_status": True,
                "pm_finish_once_normal": True,
                "pm_finish_once_early": True,
                "architecture_route": True,
                "ui_route": True,
                "mapping_and_library_precedence": True,
                "synthetic_receipt_boundary": True,
            },
        })
receipt = {
    "schema": "nxframework-firmware-matrix-receipt-v2",
    "schema_version": 2,
    "matrix_contract": matrix["contract_id"],
    "evidence": {
        "kind": "synthetic",
        "hardware_ran": False,
        "device_access": False,
        "firmware_images_used": False,
        "universal_evidence": False,
    },
    "profiles": records,
    "summary": {
        "profile_count": len(matrix["profiles"]),
        "case_count": len(records),
        "passed": len(records),
        "failed": 0,
    },
}
with open(output_path, "x", encoding="utf-8") as stream:
    json.dump(receipt, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

python3 -B "$VALIDATOR" --receipt "$RECEIPT"
if [[ -n $RECEIPT_OUTPUT ]]; then
  [[ ! -e $RECEIPT_OUTPUT && ! -L $RECEIPT_OUTPUT ]] ||
    fail "receipt output already exists: $RECEIPT_OUTPUT"
  (umask 077; cp "$RECEIPT" "$RECEIPT_OUTPUT")
fi
printf 'firmware matrix v2 process gate passed: profiles=9 cases=15 '
printf 'field_cases=5 evidence_kind=synthetic no_stat=20 early_log=15 '
printf 'pm_finish=30 hardware_ran=0 device_access=0 firmware_images_used=0\n'
