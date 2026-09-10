#!/bin/bash
# Transactional first-run installer for the user's legal Sonic 4 EP2 v3 data.
set -u
set -o pipefail

SCRIPT="${BASH_SOURCE:-$0}"
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT")" && pwd)"
if [ -n "${SONIC_GAMEDIR:-}" ]; then
  mkdir -p "$SONIC_GAMEDIR" || exit 1
  GAMEDIR="$(cd "$SONIC_GAMEDIR" && pwd)"
else
  GAMEDIR="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
cd "$GAMEDIR" || exit 1

INPUT_SOURCE="${1:-}"
RUN_TOKEN="$$.$(date +%s 2>/dev/null || printf 0).${RANDOM:-0}.${RANDOM:-0}"
BIN="${SONIC_BINARY:-$GAMEDIR/sonic4.arm64}"
VALIDATOR="${SONIC_VALIDATOR:-$SCRIPT_DIR/validate-sonic4-data.py}"
SETUPF="${SONIC_SETUP_FILE:-/tmp/sonic_setup.$RUN_TOKEN.txt}"
STOPF="${SONIC_SETUP_STOP:-/tmp/sonic_setup_stop.$RUN_TOKEN}"
BAKELOG="${SONIC_SETUP_LOG:-$GAMEDIR/bake.log}"
MARKER="$GAMEDIR/.sonic4ep2-v6-data.ok"
LOCKDIR="$GAMEDIR/.sonic4ep2-install.lock"
STAGE="$GAMEDIR/.sonic4ep2-v6-stage.$RUN_TOKEN"
BACKUP="$GAMEDIR/.sonic4ep2-v6-backup.$RUN_TOKEN"
TMPROOT="$GAMEDIR/.sonic4ep2-v6-tmp.$RUN_TOKEN"
F2F_FILE="$GAMEDIR/Sonic4ep2.f2f"

LIBFOX_SIZE=12220336
LIBFOX_SHA256=ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a
LIBFOX_BUILD_ID=fb0e884de8487a837d5fdda6ea35f731f21cff32
LIBFOX_CRC32=56c8d296
DATA_OBB_SIZE=673277896
DATA_OBB_SHA256=a2c988a0c2b057b27a328053cef1627e16b6491f5b6700f9627cdc141f82012b
DATA_OBB_CRC32=7b18b873
DATA_OBB_MAGIC=4c504b00
EXTRACTION_TOTAL_BYTES=$((LIBFOX_SIZE + DATA_OBB_SIZE))
BUNDLE_EXPANDED_BYTES=855915826
BUNDLE_MAX_UNCOMPRESSED_BYTES=1200000000
SPACE_SAFETY_BYTES=134217728

SP=""
POLL=""
LOCK_HELD=0
COMMIT_STARTED=0
COMMIT_DONE=0
LIB_SOURCE=""
LIB_ENTRY=""
LIB_SOURCE_TYPE=""
LIB_ORIGINAL=""
OBB_SOURCE=""
OBB_ENTRY=""
OBB_SOURCE_TYPE=""
OBB_ORIGINAL=""
FILE_SIZE_BACKEND=""

declare -a INSTALL_FILES=(
  "lib/arm64-v8a/libfox.so"
  "data/data.obb"
)
declare -a TOUCHED_FILES=()
declare -a ARCHIVES=()
declare -a ARCHIVE_PARENTS=()
declare -a ARCHIVE_TRUSTED=()
declare -a VALID_ARCHIVES=()
declare -a VALID_ARCHIVE_PARENTS=()
declare -a BUNDLES=()
declare -a LOOSE_LIBS=()
declare -a LOOSE_OBBS=()
declare -a USED_ORIGINALS=()

blog() {
  printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$BAKELOG"
}

is_unsigned_integer() {
  case "${1:-}" in
    ''|*[!0-9]*) return 1 ;;
    *) return 0 ;;
  esac
}

init_file_size_backend() {
  local probe=$SCRIPT size
  if command -v stat >/dev/null 2>&1; then
    size=$(stat -c '%s' "$probe" 2>/dev/null) || size=""
    is_unsigned_integer "$size" && { FILE_SIZE_BACKEND=stat-c; return 0; }
  fi
  if command -v busybox >/dev/null 2>&1; then
    size=$(busybox stat -c '%s' "$probe" 2>/dev/null) || size=""
    is_unsigned_integer "$size" && { FILE_SIZE_BACKEND=busybox-stat; return 0; }
  fi
  if command -v stat >/dev/null 2>&1; then
    size=$(stat -f '%z' "$probe" 2>/dev/null) || size=""
    is_unsigned_integer "$size" && { FILE_SIZE_BACKEND=stat-f; return 0; }
  fi
  if command -v python3 >/dev/null 2>&1; then
    size=$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' \
      "$probe" 2>/dev/null) || size=""
    is_unsigned_integer "$size" && { FILE_SIZE_BACKEND=python3; return 0; }
  fi
  if command -v wc >/dev/null 2>&1; then
    size=$(LC_ALL=C wc -c < "$probe" 2>/dev/null) || size=""
    size=${size//[[:space:]]/}
    is_unsigned_integer "$size" && { FILE_SIZE_BACKEND=wc-c; return 0; }
  fi
  return 1
}

file_size() {
  local path=$1 size
  [ -f "$path" ] || return 1
  case "$FILE_SIZE_BACKEND" in
    stat-c) size=$(stat -c '%s' "$path" 2>/dev/null) || return 1 ;;
    busybox-stat) size=$(busybox stat -c '%s' "$path" 2>/dev/null) || return 1 ;;
    stat-f) size=$(stat -f '%z' "$path" 2>/dev/null) || return 1 ;;
    python3)
      size=$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' \
        "$path" 2>/dev/null) || return 1
      ;;
    wc-c)
      size=$(LC_ALL=C wc -c < "$path" 2>/dev/null) || return 1
      size=${size//[[:space:]]/}
      ;;
    *) return 1 ;;
  esac
  is_unsigned_integer "$size" || return 1
  printf '%s\n' "$size"
}

clamp_permille() {
  local value=${1:-0}
  case "$value" in ''|*[!0-9-]*) value=0 ;; esac
  [ "$value" -ge 0 ] 2>/dev/null || value=0
  [ "$value" -le 1000 ] 2>/dev/null || value=1000
  printf '%s' "$value"
}

setup_progress() {
  local state=$1 phase=$2 validation extraction message active tmp
  validation=$(clamp_permille "$3")
  extraction=$(clamp_permille "$4")
  message=$5
  active=$validation
  [ "$phase" = 4 ] && active=$extraction
  tmp="${SETUPF}.tmp.${BASHPID:-$$}"
  if ! printf '%d %d 1000\n%s\nSONIC_SETUP_V2 %d %d %d\n' \
      "$state" "$active" "$message" "$phase" "$validation" "$extraction" \
      > "$tmp" 2>/dev/null; then
    rm -f "$tmp" 2>/dev/null || true
    return 0
  fi
  mv -f "$tmp" "$SETUPF" 2>/dev/null || rm -f "$tmp" 2>/dev/null || true
  return 0
}

setup_error() {
  local message=$1 phase=0 validation=0 extraction=0 line tag
  if [ -s "$SETUPF" ]; then
    line=$(sed -n '3p' "$SETUPF" 2>/dev/null || true)
    read -r tag phase validation extraction <<< "$line"
    if [ "$tag" != SONIC_SETUP_V2 ]; then
      phase=0
      validation=0
      extraction=0
    fi
  fi
  setup_progress 2 "$phase" "$validation" "$extraction" "$message"
}

process_running() {
  local pid=$1 state
  kill -0 "$pid" 2>/dev/null || return 1
  state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
  [ "$state" != Z ]
}

wait_process_deadline() {
  local pid=$1 seconds=$2 elapsed=0
  while process_running "$pid" && [ "$elapsed" -lt "$seconds" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
  done
  ! process_running "$pid"
}

splash_start() {
  [ -z "$SP" ] || return 0
  case "${SONIC_NO_SPLASH:-0}" in 1|on|ON|true|TRUE|yes|YES) return 0 ;; esac
  [ -x "$BIN" ] || { blog "setup splash unavailable; continuing headless"; return 0; }
  rm -f "$STOPF"
  SONIC_SETUPSPLASH=1 SONIC_SETUP_FILE="$SETUPF" SONIC_SETUP_STOP="$STOPF" \
    "$BIN" >> "$BAKELOG" 2>&1 </dev/null &
  SP=$!
  blog "setup splash pid=$SP"
}

splash_stop() {
  [ -n "$SP" ] || return 0
  touch "$STOPF" 2>/dev/null || true
  if ! wait_process_deadline "$SP" 3; then
    blog "setup splash did not stop in 3s; sending TERM"
    kill -TERM "$SP" 2>/dev/null || true
    wait_process_deadline "$SP" 1 || true
  fi
  if process_running "$SP"; then
    blog "setup splash ignored TERM; sending KILL and continuing headless"
    kill -KILL "$SP" 2>/dev/null || true
    wait_process_deadline "$SP" 1 || true
  fi
  if ! process_running "$SP"; then
    wait "$SP" 2>/dev/null || true
  else
    blog "warning: setup splash is stuck in kernel I/O; not waiting"
  fi
  rm -f "$STOPF"
  SP=""
}

stop_poll() {
  [ -n "$POLL" ] || return 0
  kill -TERM "$POLL" 2>/dev/null || true
  wait_process_deadline "$POLL" 2 || true
  if process_running "$POLL"; then
    kill -KILL "$POLL" 2>/dev/null || true
    wait_process_deadline "$POLL" 1 || true
  fi
  if ! process_running "$POLL"; then
    wait "$POLL" 2>/dev/null || true
  else
    blog "warning: progress poll is stuck; continuing without waiting"
  fi
  POLL=""
}

release_lock() {
  local owner
  [ "$LOCK_HELD" = 1 ] || return 0
  owner=$(sed -n '1p' "$LOCKDIR/pid" 2>/dev/null || true)
  if [ "$owner" = "$$" ]; then
    rm -rf "$LOCKDIR"
  else
    blog "warning: extraction lock ownership changed; not removing it"
  fi
  LOCK_HELD=0
}

acquire_lock() {
  local owner cmdline
  if mkdir "$LOCKDIR" 2>/dev/null; then
    LOCK_HELD=1
    printf '%s\n' "$$" > "$LOCKDIR/pid" || { release_lock; return 1; }
    return 0
  fi

  owner=$(sed -n '1p' "$LOCKDIR/pid" 2>/dev/null || true)
  case "$owner" in
    ''|*[!0-9]*)
      # mkdir and the owner write are separate operations. Give a concurrent
      # launcher time to publish its PID before treating this as a stale lock.
      sleep 1
      owner=$(sed -n '1p' "$LOCKDIR/pid" 2>/dev/null || true)
      ;;
  esac
  case "$owner" in
    ''|*[!0-9]*) cmdline="" ;;
    *) cmdline=$(tr '\0' ' ' < "/proc/$owner/cmdline" 2>/dev/null || true) ;;
  esac
  if [ -n "$cmdline" ] && [[ "$cmdline" == *sonic4ep2_extract.sh* ]]; then
    blog "another Sonic 4 EP2 installation is active (pid=$owner)"
    return 1
  fi

  blog "removing stale installation lock${owner:+ from pid=$owner}"
  rm -rf "$LOCKDIR" || return 1
  mkdir "$LOCKDIR" 2>/dev/null || return 1
  LOCK_HELD=1
  printf '%s\n' "$$" > "$LOCKDIR/pid" || { release_lock; return 1; }
}

rollback_commit() {
  local rel failed=0
  [ "$COMMIT_STARTED" = 1 ] || return 0
  [ "$COMMIT_DONE" = 0 ] || return 0
  blog "rolling back interrupted payload commit"
  for rel in "${TOUCHED_FILES[@]}"; do
    rm -f "$GAMEDIR/$rel" || failed=1
  done
  for rel in "${INSTALL_FILES[@]}"; do
    if [ -e "$BACKUP/$rel" ]; then
      mkdir -p "$(dirname "$GAMEDIR/$rel")" || failed=1
      mv -f "$BACKUP/$rel" "$GAMEDIR/$rel" 2>> "$BAKELOG" || failed=1
    fi
  done
  if [ -f "$BACKUP/marker" ]; then
    rm -f "$MARKER" || failed=1
    mv -f "$BACKUP/marker" "$MARKER" 2>> "$BAKELOG" || failed=1
  elif [ ! -f "$BACKUP/previous-marker" ]; then
    rm -f "$MARKER" || failed=1
  fi
  sync
  [ "$failed" = 0 ]
}

cleanup() {
  local rc=$? rollback_ok=1
  trap - EXIT INT TERM HUP
  stop_poll
  rollback_commit || rollback_ok=0
  rm -rf "$STAGE" "$TMPROOT"
  if [ "$rollback_ok" = 1 ]; then
    rm -rf "$BACKUP"
  else
    blog "ERROR: rollback incomplete; recovery preserved at $BACKUP"
  fi
  splash_stop
  release_lock
  rm -f "$SETUPF" "$STOPF"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail_extract() {
  local message="${1:-INSTALLATION FAILED}"
  blog "ERROR: $message"
  stop_poll
  setup_error "$message"
  splash_start
  sleep "${SONIC_SETUP_ERROR_DELAY:-5}"
  exit 1
}

require_tools() {
  local command
  for command in unzip sha256sum od dd; do
    command -v "$command" >/dev/null 2>&1 \
      || fail_extract "MISSING REQUIRED TOOL: $command"
  done
  if ! command -v python3 >/dev/null 2>&1 || [ ! -s "$VALIDATOR" ]; then
    blog "Python validator unavailable; CRC/hash shell fallback enabled"
  fi
  init_file_size_backend || fail_extract "NO PORTABLE FILE-SIZE BACKEND"
  blog "file-size backend: $FILE_SIZE_BACKEND"
}

ensure_f2f_file() {
  local tmp
  [ -e "$F2F_FILE" ] && return 0
  tmp="${F2F_FILE}.tmp.$RUN_TOKEN"
  printf '%s\n' '{"MerchandiseTime":1782470230}' > "$tmp" || return 1
  if [ ! -e "$F2F_FILE" ]; then
    mv -f "$tmp" "$F2F_FILE" || return 1
  else
    rm -f "$tmp"
  fi
  return 0
}

payload_sizes_exact() {
  local root=$1 lib_size obb_size
  [ -f "$root/lib/arm64-v8a/libfox.so" ] || return 1
  [ -f "$root/data/data.obb" ] || return 1
  lib_size=$(file_size "$root/lib/arm64-v8a/libfox.so") || return 1
  obb_size=$(file_size "$root/data/data.obb") || return 1
  [ "$lib_size" = "$LIBFOX_SIZE" ] && [ "$obb_size" = "$DATA_OBB_SIZE" ]
}

marker_is_valid_fast() {
  [ -s "$MARKER" ] || return 1
  grep -Fxq 'format=1' "$MARKER" || return 1
  grep -Fxq "libfox_size=$LIBFOX_SIZE" "$MARKER" || return 1
  grep -Fxq "libfox_sha256=$LIBFOX_SHA256" "$MARKER" || return 1
  grep -Fxq "data_obb_size=$DATA_OBB_SIZE" "$MARKER" || return 1
  grep -Fxq "data_obb_sha256=$DATA_OBB_SHA256" "$MARKER" || return 1
  payload_sizes_exact "$GAMEDIR"
}

write_marker() {
  local root=$1 marker tmp
  marker="$root/.sonic4ep2-v6-data.ok"
  tmp="${marker}.tmp.$RUN_TOKEN"
  {
    echo 'format=1'
    echo 'port=sonic4ep2-v6'
    echo "libfox_size=$LIBFOX_SIZE"
    echo "libfox_sha256=$LIBFOX_SHA256"
    echo "libfox_buildid=$LIBFOX_BUILD_ID"
    echo "data_obb_size=$DATA_OBB_SIZE"
    echo "data_obb_sha256=$DATA_OBB_SHA256"
    echo "data_obb_magic=$DATA_OBB_MAGIC"
  } > "$tmp" || return 1
  chmod 644 "$tmp" 2>/dev/null || true
  mv -f "$tmp" "$marker"
}

validate_payload_shell() {
  local root=$1 lib obb size digest magic elf_id build_id
  lib="$root/lib/arm64-v8a/libfox.so"
  obb="$root/data/data.obb"
  payload_sizes_exact "$root" || return 1

  digest=$(sha256sum "$lib" 2>> "$BAKELOG" | awk '{print $1}') || return 1
  [ "$digest" = "$LIBFOX_SHA256" ] || return 1
  elf_id=$(dd if="$lib" bs=1 count=20 2>/dev/null | od -An -tx1 | tr -d ' \n')
  case "$elf_id" in
    7f454c46020101??????????????????????b700) ;;
    *) blog "libfox.so is not little-endian AArch64 ELF64"; return 1 ;;
  esac
  if command -v readelf >/dev/null 2>&1; then
    build_id=$(readelf -n "$lib" 2>/dev/null \
      | sed -n 's/^[[:space:]]*Build ID:[[:space:]]*//p' | head -n 1)
    [ "$build_id" = "$LIBFOX_BUILD_ID" ] || return 1
  else
    blog "readelf unavailable; exact libfox SHA256 implies BuildID $LIBFOX_BUILD_ID"
  fi

  magic=$(dd if="$obb" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
  [ "$magic" = "$DATA_OBB_MAGIC" ] || return 1
  digest=$(sha256sum "$obb" 2>> "$BAKELOG" | awk '{print $1}') || return 1
  [ "$digest" = "$DATA_OBB_SHA256" ]
}

validate_payload() {
  local root=$1 base=${2:-0} span=${3:-1000} extraction=${4:-1000} end
  end=$((base + span))
  setup_progress 1 5 "$base" "$extraction" "VALIDATING INSTALLED GAME DATA"
  if command -v python3 >/dev/null 2>&1 && [ -s "$VALIDATOR" ]; then
    if SONIC_SETUP_PROGRESS_FILE="$SETUPF" \
       SONIC_SETUP_PHASE=5 \
       SONIC_VALIDATION_BASE="$base" \
       SONIC_VALIDATION_SPAN="$span" \
       SONIC_EXTRACTION_PERMILLE="$extraction" \
       SONIC_SETUP_MESSAGE="VALIDATING INSTALLED GAME DATA" \
       python3 "$VALIDATOR" payload "$root/lib/arm64-v8a/libfox.so" \
         "$root/data/data.obb" >> "$BAKELOG" 2>&1; then
      setup_progress 1 5 "$end" "$extraction" "GAME DATA VALIDATED"
      return 0
    fi
    blog "Python payload validation failed; confirming with shell tools"
  fi
  setup_progress 1 5 "$base" "$extraction" "VALIDATING DATA (SYSTEM FALLBACK)"
  validate_payload_shell "$root" || return 1
  setup_progress 1 5 "$end" "$extraction" "GAME DATA VALIDATED"
  return 0
}

recover_orphan_backup() {
  local backup=$1 rel failed=0 previous has_payload=0
  previous="$backup/previous-files"
  if marker_is_valid_fast; then
    blog "removing completed orphan transaction: $backup"
    rm -rf "$backup"
    return 0
  fi
  if [ ! -f "$previous" ]; then
    for rel in "${INSTALL_FILES[@]}" marker previous-marker; do
      if [ -e "$backup/$rel" ]; then has_payload=1; break; fi
    done
    if [ "$has_payload" = 0 ]; then
      rm -rf "$backup"
      return 0
    fi
    blog "WARNING: unknown backup layout preserved: $backup"
    return 1
  fi
  while IFS= read -r rel || [ -n "$rel" ]; do
    [ -n "$rel" ] || continue
    case " ${INSTALL_FILES[*]} " in
      *" $rel "*) ;;
      *) blog "WARNING: unsafe orphan manifest preserved: $backup"; return 1 ;;
    esac
  done < "$previous"

  blog "recovering interrupted payload transaction: $backup"
  for rel in "${INSTALL_FILES[@]}"; do
    if [ -e "$backup/$rel" ]; then
      rm -f "$GAMEDIR/$rel" || failed=1
      mkdir -p "$(dirname "$GAMEDIR/$rel")" || failed=1
      mv -f "$backup/$rel" "$GAMEDIR/$rel" 2>> "$BAKELOG" || failed=1
    elif ! grep -Fxq "$rel" "$previous"; then
      rm -f "$GAMEDIR/$rel" || failed=1
    fi
  done
  if [ -f "$backup/marker" ]; then
    rm -f "$MARKER" || failed=1
    mv -f "$backup/marker" "$MARKER" 2>> "$BAKELOG" || failed=1
  elif [ ! -f "$backup/previous-marker" ]; then
    rm -f "$MARKER" || failed=1
  fi
  if [ "$failed" = 0 ]; then
    sync
    rm -rf "$backup"
    blog "orphan transaction recovered"
    return 0
  fi
  blog "ERROR: orphan recovery incomplete; preserved $backup"
  return 1
}

recover_orphan_transactions() {
  local path
  shopt -s nullglob
  for path in "$GAMEDIR"/.sonic4ep2-v6-backup.*; do
    recover_orphan_backup "$path" || { shopt -u nullglob; return 1; }
  done
  for path in "$GAMEDIR"/.sonic4ep2-v6-stage.* \
              "$GAMEDIR"/.sonic4ep2-v6-tmp.*; do
    blog "removing orphan installation workspace: $path"
    rm -rf "$path" || { shopt -u nullglob; return 1; }
  done
  shopt -u nullglob
  return 0
}

add_unique() {
  local array_name=$1 value=$2 current
  local -n target=$array_name
  [ -f "$value" ] || return 0
  for current in "${target[@]}"; do [ "$current" = "$value" ] && return 0; done
  target+=("$value")
}

add_archive() {
  local path=$1 parent=${2:-$1} trusted=${3:-0} current index=0
  [ -f "$path" ] || return 0
  for current in "${ARCHIVES[@]}"; do
    if [ "$current" = "$path" ]; then
      [ "$trusted" = 1 ] && ARCHIVE_TRUSTED[$index]=1
      return 0
    fi
    index=$((index + 1))
  done
  ARCHIVES+=("$path")
  ARCHIVE_PARENTS+=("$parent")
  ARCHIVE_TRUSTED+=("$trusted")
}

add_used_original() {
  local source=$1 current
  [ -f "$source" ] || return 0
  for current in "${USED_ORIGINALS[@]}"; do
    [ "$current" = "$source" ] && return 0
  done
  USED_ORIGINALS+=("$source")
}

gather_sources() {
  local dir source lower
  shopt -s nullglob
  if [ -n "$INPUT_SOURCE" ] && [ -f "$INPUT_SOURCE" ]; then
    lower=${INPUT_SOURCE,,}
    case "$lower" in
      *.apkm|*.apks) add_unique BUNDLES "$INPUT_SOURCE" ;;
      *.apk) add_archive "$INPUT_SOURCE" "$INPUT_SOURCE" 0 ;;
      *.obb) add_unique LOOSE_OBBS "$INPUT_SOURCE" ;;
      *libfox.so) add_unique LOOSE_LIBS "$INPUT_SOURCE" ;;
    esac
  fi
  for dir in "$GAMEDIR/gamedata" "$GAMEDIR"; do
    [ -d "$dir" ] || continue
    for source in "$dir"/*.apk "$dir"/*.APK; do
      add_archive "$source" "$source" 0
    done
    for source in "$dir"/*.apkm "$dir"/*.APKM "$dir"/*.apks "$dir"/*.APKS; do
      add_unique BUNDLES "$source"
    done
    for source in "$dir"/*.obb "$dir"/*.OBB; do
      add_unique LOOSE_OBBS "$source"
    done
    for source in "$dir"/libfox.so; do
      add_unique LOOSE_LIBS "$source"
    done
  done
  shopt -u nullglob
}

existing_payload_bytes() {
  local rel size total=0
  for rel in "${INSTALL_FILES[@]}"; do
    [ -f "$GAMEDIR/$rel" ] || continue
    size=$(file_size "$GAMEDIR/$rel" 2>/dev/null) || continue
    total=$((total + size))
  done
  printf '%s\n' "$total"
}

available_space_bytes() {
  local blocks
  blocks=$(df -Pk "$GAMEDIR" 2>/dev/null \
    | awk 'NR == 2 && $4 ~ /^[0-9]+$/ { print $4; exit }')
  case "$blocks" in ''|*[!0-9]*) return 1 ;; esac
  printf '%s\n' "$((blocks * 1024))"
}

preflight_space() {
  local bundle_bytes backup_bytes required available need_mb have_mb
  bundle_bytes=$((BUNDLE_EXPANDED_BYTES * ${#BUNDLES[@]}))
  backup_bytes=$(existing_payload_bytes)
  # Existing payload files are renamed into BACKUP on the same filesystem;
  # rename consumes no additional data blocks, so they are not part of free-space need.
  required=$((EXTRACTION_TOTAL_BYTES + bundle_bytes + SPACE_SAFETY_BYTES))
  available=$(available_space_bytes) \
    || fail_extract "CANNOT DETERMINE FREE SPACE FOR INSTALLATION"
  need_mb=$(((required + 1048575) / 1048576))
  have_mb=$((available / 1048576))
  blog "space preflight: need=${need_mb}MB available=${have_mb}MB "\
"(bundle=$((bundle_bytes / 1048576))MB stage=$((EXTRACTION_TOTAL_BYTES / 1048576))MB "\
"existing-to-rename=$((backup_bytes / 1048576))MB safety=$((SPACE_SAFETY_BYTES / 1048576))MB)"
  [ "$available" -ge "$required" ] \
    || fail_extract "NOT ENOUGH FREE SPACE: NEED ${need_mb} MB, HAVE ${have_mb} MB"
}

find_archive_entry() {
  local archive=$1 basename=$2 result
  if command -v python3 >/dev/null 2>&1 && [ -s "$VALIDATOR" ]; then
    python3 "$VALIDATOR" find-entry "$archive" "$basename" 2>> "$BAKELOG"
    return $?
  fi
  result=$(unzip -l "$archive" 2>/dev/null \
    | awk -v base="$basename" '
        $1 ~ /^[0-9]+$/ {
          name=$NF; count=split(name, parts, "/")
          if (parts[count] == base) {
            if (found) duplicate=1
            else { value=name; found=1 }
          }
        }
        END { if (found && !duplicate) print value; else exit 1 }') \
    || return 1
  printf '%s\n' "$result"
}

archive_uncompressed_size() {
  unzip -l "$1" 2>/dev/null \
    | awk '
        $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && ($3 == "file" || $3 == "files") {
          summary=$1; next
        }
        $1 ~ /^[0-9]+$/ &&
        $2 ~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$/ &&
        $3 ~ /^[0-9][0-9]:[0-9][0-9]$/ {
          total += $1; members++
        }
        END {
          if (summary != "") print summary
          else if (members > 0) printf "%.0f\n", total
          else exit 1
        }'
}

archive_entry_info() {
  local archive=$1 entry=$2
  if command -v python3 >/dev/null 2>&1 && [ -s "$VALIDATOR" ]; then
    # Missing entries are normal while inspecting separate ABI/data splits.
    python3 "$VALIDATOR" entry-info "$archive" "$entry" 2>/dev/null
    return $?
  fi
  unzip -v "$archive" "$entry" 2>/dev/null \
    | awk -v wanted="$entry" '$8 == wanted { print $1, tolower($7); found=1; exit } END { if (!found) exit 1 }'
}

archive_entry_size() {
  unzip -l "$1" "$2" 2>/dev/null \
    | awk -v wanted="$2" '$1 ~ /^[0-9]+$/ && $4 == wanted { print $1; found=1; exit } END { if (!found) exit 1 }'
}

archive_entry_exact() {
  local archive=$1 entry=$2 expected_size=$3 expected_crc=$4 expected_sha=$5
  local info size crc digest
  info=$(archive_entry_info "$archive" "$entry" 2>/dev/null || true)
  read -r size crc <<< "$info"
  if [ -n "${size:-}" ] && [ -n "${crc:-}" ]; then
    [ "$size" = "$expected_size" ] && [ "${crc,,}" = "$expected_crc" ]
    return $?
  fi
  size=$(archive_entry_size "$archive" "$entry") || return 1
  [ "$size" = "$expected_size" ] || return 1
  blog "entry CRC metadata unavailable; checking SHA256 through unzip: $(basename "$archive")"
  digest=$(unzip -p "$archive" "$entry" 2>> "$BAKELOG" \
    | sha256sum | awk '{print $1}') || return 1
  [ "$digest" = "$expected_sha" ]
}

validate_archive_source() {
  local source=$1 base=$2 span=$3 message=$4 end unzip_help
  end=$((base + span))
  setup_progress 1 2 "$base" 0 "$message"
  if command -v python3 >/dev/null 2>&1 && [ -s "$VALIDATOR" ]; then
    if SONIC_SETUP_PROGRESS_FILE="$SETUPF" \
       SONIC_SETUP_PHASE=2 \
       SONIC_VALIDATION_BASE="$base" \
       SONIC_VALIDATION_SPAN="$span" \
       SONIC_EXTRACTION_PERMILLE=0 \
       SONIC_SETUP_MESSAGE="$message" \
       python3 "$VALIDATOR" archive "$source" >> "$BAKELOG" 2>&1; then
      setup_progress 1 2 "$end" 0 "$message"
      return 0
    fi
    blog "Python ZIP validation failed; retrying with system unzip: $source"
  fi
  setup_progress 1 2 "$base" 0 "$message (SYSTEM FALLBACK)"
  unzip_help=$(unzip -h 2>&1 || true)
  case "$unzip_help" in
  *-t*)
    if unzip -tq "$source" </dev/null >> "$BAKELOG" 2>&1 \
       || unzip -t "$source" </dev/null >> "$BAKELOG" 2>&1; then
      setup_progress 1 2 "$end" 0 "$message"
      return 0
    fi
    ;;
  *)
    blog "system unzip has no -t; validating every member through unzip -p"
    if unzip -p "$source" </dev/null > /dev/null 2>> "$BAKELOG"; then
      setup_progress 1 2 "$end" 0 "$message"
      return 0
    fi
    ;;
  esac
  return 1
}

extract_archive_entry() {
  local source=$1 entry=$2 target=$3 tmp="${3}.part.$RUN_TOKEN"
  mkdir -p "$(dirname "$target")" || return 1
  rm -f "$tmp"
  if ! unzip -p "$source" "$entry" > "$tmp" 2>> "$BAKELOG"; then
    rm -f "$tmp"
    return 1
  fi
  [ -s "$tmp" ] || { rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$target"
}

expand_bundle() {
  local bundle=$1 index=$2 base=$3 span=$4 end sub lib_entry packs_entry
  local lib_size packs_size total_size
  end=$((base + span))
  sub="$TMPROOT/bundle_$index"
  mkdir -p "$sub" || return 1
  setup_progress 1 2 "$base" 0 "VALIDATING APK BUNDLE $((index + 1)) / ${#BUNDLES[@]}"
  if command -v python3 >/dev/null 2>&1 && [ -s "$VALIDATOR" ]; then
    if SONIC_SETUP_PROGRESS_FILE="$SETUPF" \
       SONIC_SETUP_PHASE=2 \
       SONIC_VALIDATION_BASE="$base" \
       SONIC_VALIDATION_SPAN="$span" \
       SONIC_EXTRACTION_PERMILLE=0 \
       SONIC_SETUP_MESSAGE="VALIDATING APK BUNDLE $((index + 1)) / ${#BUNDLES[@]}" \
       python3 "$VALIDATOR" bundle "$bundle" "$sub" >> "$BAKELOG" 2>&1; then
      add_archive "$sub/split_config.arm64_v8a.apk" "$bundle" 1
      add_archive "$sub/split_packs.apk" "$bundle" 1
      setup_progress 1 2 "$end" 0 "APK BUNDLE VALIDATED"
      return 0
    fi
    blog "Python bundle validation failed; retrying with system unzip: $bundle"
    rm -rf "$sub"
    mkdir -p "$sub" || return 1
  fi
  lib_entry=$(find_archive_entry "$bundle" split_config.arm64_v8a.apk) || return 1
  packs_entry=$(find_archive_entry "$bundle" split_packs.apk) || return 1
  lib_size=$(archive_entry_size "$bundle" "$lib_entry") || return 1
  packs_size=$(archive_entry_size "$bundle" "$packs_entry") || return 1
  [ "$lib_size" = 12247458 ] || { blog "rejected bundle: ARM64 split size=$lib_size"; return 1; }
  [ "$packs_size" = 843668368 ] || { blog "rejected bundle: packs split size=$packs_size"; return 1; }
  total_size=$(archive_uncompressed_size "$bundle") || return 1
  [ "$total_size" -le "$BUNDLE_MAX_UNCOMPRESSED_BYTES" ] \
    || { blog "rejected bundle: uncompressed size=$total_size exceeds safety cap"; return 1; }
  validate_archive_source "$bundle" "$base" "$span" \
    "VALIDATING APK BUNDLE $((index + 1)) / ${#BUNDLES[@]}" || return 1
  extract_archive_entry "$bundle" "$lib_entry" "$sub/split_config.arm64_v8a.apk" || return 1
  extract_archive_entry "$bundle" "$packs_entry" "$sub/split_packs.apk" || return 1
  add_archive "$sub/split_config.arm64_v8a.apk" "$bundle" 1
  add_archive "$sub/split_packs.apk" "$bundle" 1
  return 0
}

prepare_and_validate_archives() {
  local count index base end span source start untrusted=0
  count=${#BUNDLES[@]}
  if [ "$count" -gt 0 ]; then
    for ((index=0; index<count; index++)); do
      base=$((index * 200 / count))
      end=$(((index + 1) * 200 / count))
      span=$((end - base))
      expand_bundle "${BUNDLES[$index]}" "$index" "$base" "$span" \
        || blog "rejected damaged/incomplete bundle: ${BUNDLES[$index]}"
    done
    start=200
  else
    start=0
  fi

  for ((index=0; index<${#ARCHIVES[@]}; index++)); do
    [ "${ARCHIVE_TRUSTED[$index]}" = 1 ] || untrusted=$((untrusted + 1))
  done
  count=0
  for ((index=0; index<${#ARCHIVES[@]}; index++)); do
    source=${ARCHIVES[$index]}
    if [ "${ARCHIVE_TRUSTED[$index]}" = 1 ]; then
      VALID_ARCHIVES+=("$source")
      VALID_ARCHIVE_PARENTS+=("${ARCHIVE_PARENTS[$index]}")
      continue
    fi
    base=$((start + count * (450 - start) / (untrusted > 0 ? untrusted : 1)))
    end=$((start + (count + 1) * (450 - start) / (untrusted > 0 ? untrusted : 1)))
    span=$((end - base))
    if validate_archive_source "$source" "$base" "$span" \
       "VALIDATING APK $((count + 1)) / $untrusted"; then
      VALID_ARCHIVES+=("$source")
      VALID_ARCHIVE_PARENTS+=("${ARCHIVE_PARENTS[$index]}")
    else
      blog "rejected damaged APK: $source"
    fi
    count=$((count + 1))
  done
  setup_progress 1 3 450 0 "SELECTING SONIC 4 EP2 V3 DATA"
}

loose_file_exact() {
  local file=$1 expected_size=$2 expected_sha=$3 size digest
  size=$(file_size "$file") || return 1
  [ "$size" = "$expected_size" ] || return 1
  digest=$(sha256sum "$file" 2>> "$BAKELOG" | awk '{print $1}') || return 1
  [ "$digest" = "$expected_sha" ]
}

select_sources() {
  local index source parent candidate
  for ((index=0; index<${#VALID_ARCHIVES[@]}; index++)); do
    source=${VALID_ARCHIVES[$index]}
    parent=${VALID_ARCHIVE_PARENTS[$index]}
    if [ -z "$LIB_SOURCE" ] && archive_entry_exact "$source" \
       'lib/arm64-v8a/libfox.so' "$LIBFOX_SIZE" "$LIBFOX_CRC32" "$LIBFOX_SHA256"; then
      LIB_SOURCE=$source
      LIB_ENTRY='lib/arm64-v8a/libfox.so'
      LIB_SOURCE_TYPE=archive
      LIB_ORIGINAL=$parent
    fi
    if [ -z "$OBB_SOURCE" ] && archive_entry_exact "$source" \
       'assets/data.obb' "$DATA_OBB_SIZE" "$DATA_OBB_CRC32" "$DATA_OBB_SHA256"; then
      OBB_SOURCE=$source
      OBB_ENTRY='assets/data.obb'
      OBB_SOURCE_TYPE=archive
      OBB_ORIGINAL=$parent
    fi
  done
  if [ -z "$LIB_SOURCE" ]; then
    for candidate in "${LOOSE_LIBS[@]}"; do
      if loose_file_exact "$candidate" "$LIBFOX_SIZE" "$LIBFOX_SHA256"; then
        LIB_SOURCE=$candidate; LIB_SOURCE_TYPE=loose; LIB_ORIGINAL=$candidate; break
      fi
    done
  fi
  if [ -z "$OBB_SOURCE" ]; then
    for candidate in "${LOOSE_OBBS[@]}"; do
      if loose_file_exact "$candidate" "$DATA_OBB_SIZE" "$DATA_OBB_SHA256"; then
        OBB_SOURCE=$candidate; OBB_SOURCE_TYPE=loose; OBB_ORIGINAL=$candidate; break
      fi
    done
  fi
  [ -n "$LIB_SOURCE" ] && [ -n "$OBB_SOURCE" ] || return 1
  add_used_original "$LIB_ORIGINAL"
  add_used_original "$OBB_ORIGINAL"
  setup_progress 1 3 500 0 "SONIC 4 EP2 V3 DATA SELECTED"
  blog "libfox source: $LIB_ORIGINAL"
  blog "data.obb source: $OBB_ORIGINAL"
}

copy_loose_file() {
  local source=$1 target=$2 tmp="${2}.part.$RUN_TOKEN"
  mkdir -p "$(dirname "$target")" || return 1
  rm -f "$tmp"
  cp -f "$source" "$tmp" 2>> "$BAKELOG" || { rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$target"
}

stage_payload_bytes() {
  local file size total=0
  local -a files=()
  shopt -s nullglob
  files=("$STAGE"/lib/arm64-v8a/libfox.so* "$STAGE"/data/data.obb*)
  shopt -u nullglob
  for file in "${files[@]}"; do
    [ -f "$file" ] || continue
    size=$(file_size "$file" 2>/dev/null) || continue
    total=$((total + size))
  done
  printf '%s\n' "$total"
}

start_progress_poll() {
  (
    local done_bytes extraction done_mb total_mb
    while :; do
      done_bytes=$(stage_payload_bytes)
      [ "$done_bytes" -le "$EXTRACTION_TOTAL_BYTES" ] || done_bytes=$EXTRACTION_TOTAL_BYTES
      extraction=$((done_bytes * 1000 / EXTRACTION_TOTAL_BYTES))
      done_mb=$((done_bytes / 1048576))
      total_mb=$((EXTRACTION_TOTAL_BYTES / 1048576))
      setup_progress 1 4 500 "$extraction" \
        "EXTRACTING GAME DATA $done_mb / $total_mb MB"
      sleep 1
    done
  ) &
  POLL=$!
}

extract_selected_payload() {
  if [ "$LIB_SOURCE_TYPE" = archive ]; then
    extract_archive_entry "$LIB_SOURCE" "$LIB_ENTRY" \
      "$STAGE/lib/arm64-v8a/libfox.so" || return 1
  else
    copy_loose_file "$LIB_SOURCE" "$STAGE/lib/arm64-v8a/libfox.so" || return 1
  fi
  if [ "$OBB_SOURCE_TYPE" = archive ]; then
    extract_archive_entry "$OBB_SOURCE" "$OBB_ENTRY" \
      "$STAGE/data/data.obb" || return 1
  else
    copy_loose_file "$OBB_SOURCE" "$STAGE/data/data.obb" || return 1
  fi
}

commit_payload() {
  local rel
  mkdir -p "$BACKUP/lib/arm64-v8a" "$BACKUP/data" \
           "$GAMEDIR/lib/arm64-v8a" "$GAMEDIR/data" || return 1
  : > "$BACKUP/previous-files" || return 1
  for rel in "${INSTALL_FILES[@]}"; do
    [ -e "$STAGE/$rel" ] || return 1
    if [ -e "$GAMEDIR/$rel" ]; then
      printf '%s\n' "$rel" >> "$BACKUP/previous-files" || return 1
    fi
  done
  if [ -f "$MARKER" ]; then
    : > "$BACKUP/previous-marker" || return 1
  fi
  sync || return 1
  COMMIT_STARTED=1
  if [ -f "$MARKER" ]; then
    mv -f "$MARKER" "$BACKUP/marker" || return 1
  fi
  for rel in "${INSTALL_FILES[@]}"; do
    if [ -e "$GAMEDIR/$rel" ]; then
      mkdir -p "$(dirname "$BACKUP/$rel")" || return 1
      mv -f "$GAMEDIR/$rel" "$BACKUP/$rel" || return 1
    fi
  done
  sync || return 1
  for rel in "${INSTALL_FILES[@]}"; do
    TOUCHED_FILES+=("$rel")
    mkdir -p "$(dirname "$GAMEDIR/$rel")" || return 1
    mv -f "$STAGE/$rel" "$GAMEDIR/$rel" || return 1
  done
  sync || return 1
  write_marker "$GAMEDIR" || return 1
  sync || return 1
  COMMIT_DONE=1
  rm -rf "$BACKUP"
  return 0
}

delete_used_sources() {
  local source source_dir canonical_source gamedir_physical
  gamedir_physical=$(cd "$GAMEDIR" 2>/dev/null && pwd -P) || return 1
  case "${SONIC_DELETE_SOURCE_AFTER_EXTRACT:-1}" in
    0|off|OFF|false|FALSE|no|NO)
      blog "source cleanup disabled by SONIC_DELETE_SOURCE_AFTER_EXTRACT"
      return 0
      ;;
  esac
  for source in "${USED_ORIGINALS[@]}"; do
    [ -f "$source" ] || continue
    source_dir=$(cd "$(dirname "$source")" 2>/dev/null && pwd -P) || {
      blog "preserved source whose directory cannot be canonicalized: $source"
      continue
    }
    canonical_source="$source_dir/$(basename "$source")"
    case "$canonical_source" in
      "$gamedir_physical"/*) ;;
      *)
        blog "preserved external source after commit: $source"
        continue
        ;;
    esac
    case "$source" in
      "$GAMEDIR/lib/arm64-v8a/libfox.so"|"$GAMEDIR/data/data.obb") continue ;;
    esac
    if rm -f "$source" 2>> "$BAKELOG"; then
      blog "deleted used source after validated commit: $source"
    else
      blog "warning: could not delete used source: $source"
    fi
  done
}

acquire_lock || fail_extract "ANOTHER INSTALLATION IS ACTIVE"
: > "$BAKELOG" || fail_extract "CANNOT WRITE SETUP LOG"
blog "=== Sonic 4 EP2 v6 setup start (gamedir=$GAMEDIR) ==="
blog "input=${INPUT_SOURCE:-<scan>} setup_file=$SETUPF"
require_tools
recover_orphan_transactions \
  || fail_extract "INTERRUPTED INSTALL RECOVERY FAILED (SEE bake.log)"
ensure_f2f_file || fail_extract "CANNOT CREATE Sonic4ep2.f2f"

# Fast path: a prior full validation wrote the canonical marker. No APK read.
if marker_is_valid_fast; then
  blog "validated marker and exact payload sizes already present"
  exit 0
fi

setup_progress 1 1 0 0 "LOOKING FOR SONIC 4 EP2 V3 FILES"
splash_start

# Adopt exact existing data when an old/missing marker is the only problem.
if payload_sizes_exact "$GAMEDIR"; then
  blog "exact-size existing payload found; validating for marker adoption"
  if validate_payload "$GAMEDIR" 0 1000 0; then
    write_marker "$GAMEDIR" || fail_extract "CANNOT WRITE VALIDATION MARKER"
    sync
    setup_progress 3 8 1000 1000 "GAME DATA READY"
    blog "existing exact payload adopted without requiring or deleting an APK"
    sleep "${SONIC_SETUP_SUCCESS_DELAY:-1}"
    exit 0
  fi
  blog "existing exact-size payload failed validation; looking for a clean source"
fi

rm -rf "$STAGE" "$BACKUP" "$TMPROOT"
mkdir -p "$STAGE/lib/arm64-v8a" "$STAGE/data" "$TMPROOT" \
  || fail_extract "CANNOT CREATE INSTALLATION STAGING"
gather_sources
if [ "${#ARCHIVES[@]}" -eq 0 ] && [ "${#BUNDLES[@]}" -eq 0 ] \
   && [ "${#LOOSE_LIBS[@]}" -eq 0 ] && [ "${#LOOSE_OBBS[@]}" -eq 0 ]; then
  fail_extract "COPY SONIC 4 EP2 V3 ARM64 APK/APKM TO THE GAME FOLDER"
fi
preflight_space

setup_progress 1 2 0 0 "VALIDATING APK INTEGRITY"
prepare_and_validate_archives
select_sources \
  || fail_extract "NO EXACT SONIC 4 EP2 V3 ARM64 LIBRARY AND DATA FOUND"

setup_progress 1 4 500 0 "EXTRACTING GAME DATA 0 MB"
start_progress_poll
extract_selected_payload || fail_extract "FAILED TO EXTRACT SELECTED GAME DATA"
stop_poll
setup_progress 1 5 500 1000 "VALIDATING EXTRACTED GAME DATA"
validate_payload "$STAGE" 500 500 1000 \
  || fail_extract "EXTRACTED DATA VALIDATION FAILED (SEE bake.log)"

setup_progress 1 7 1000 1000 "INSTALLING VALIDATED GAME DATA"
commit_payload || fail_extract "FAILED TO COMMIT VALIDATED GAME DATA"
delete_used_sources
setup_progress 3 8 1000 1000 "GAME DATA READY"
blog "=== Sonic 4 EP2 v6 setup complete ==="
sleep "${SONIC_SETUP_SUCCESS_DELAY:-1}"
exit 0
