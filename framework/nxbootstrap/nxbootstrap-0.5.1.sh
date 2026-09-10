#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Shared pre-main lifecycle for PortMaster ports. This file is vendored inside
# the port directory and sourced by the single visible launcher; game/engine
# behavior belongs in the compiled loader, not here.

if [[ ${NXBOOTSTRAP_LIBRARY_LOADED:-0} == 1 ]] &&
   [[ ${NXBOOTSTRAP_VERSION:-} == 0.5.1 ]] &&
   [[ ${NXBOOTSTRAP_LIBRARY_GENERATION:-} == nxbootstrap-library-v501 ]] &&
   declare -F nxbootstrap_main >/dev/null 2>&1; then
  return 0 2>/dev/null || exit 0
fi
NXBOOTSTRAP_VERSION=0.5.1
NXBOOTSTRAP_LIBRARY_GENERATION=nxbootstrap-library-v501
unset NXBOOTSTRAP_LIBRARY_LOADED NXBOOTSTRAP_CHILD_PID \
  NXBOOTSTRAP_CHILD_STARTTIME NXBOOTSTRAP_PHASE_PID \
  NXBOOTSTRAP_PHASE_STARTTIME NXBOOTSTRAP_FINISHED \
  NXBOOTSTRAP_PLATFORM_PREPARED NXBOOTSTRAP_LOCK_KIND \
  NXBOOTSTRAP_LOCK_OWNER NXBOOTSTRAP_LOCK_PATH NXBOOTSTRAP_STATUS \
  NXBOOTSTRAP_CLEANING NXBOOTSTRAP_CLEANED NXBOOTSTRAP_LOG_FD \
  NXBOOTSTRAP_INHERITED_LD_LIBRARY_PATH \
  NXBOOTSTRAP_INHERITED_LD_CAPTURED NXBOOTSTRAP_PROC_ROOT \
  NXBOOTSTRAP_GAME_ROOT NXBOOTSTRAP_BIN NXBOOTSTRAP_LOG \
  NXBOOTSTRAP_LOCK_BASE NXBOOTSTRAP_LIBRARY_PATH \
  NXBOOTSTRAP_CURRENT_PHASE 2>/dev/null || true
NXBOOTSTRAP_LIBRARY_LOADED=1

NXBOOTSTRAP_CHILD_PID=
NXBOOTSTRAP_CHILD_STARTTIME=
NXBOOTSTRAP_PHASE_PID=
NXBOOTSTRAP_PHASE_STARTTIME=
NXBOOTSTRAP_FINISHED=0
NXBOOTSTRAP_PLATFORM_PREPARED=0
NXBOOTSTRAP_LOCK_KIND=
NXBOOTSTRAP_LOCK_OWNER=
NXBOOTSTRAP_LOCK_PATH=
NXBOOTSTRAP_STATUS=0
NXBOOTSTRAP_CLEANING=0
NXBOOTSTRAP_CLEANED=0
NXBOOTSTRAP_LOG_FD=
NXBOOTSTRAP_INHERITED_LD_LIBRARY_PATH=
NXBOOTSTRAP_INHERITED_LD_CAPTURED=0
NXBOOTSTRAP_CURRENT_PHASE=library-loaded
# Never inherit a process-filesystem override from the launcher environment.
# Tests that source this library may replace this variable only after sourcing,
# inside their own process. A public launcher always starts from real procfs.
NXBOOTSTRAP_PROC_ROOT=/proc

nxbootstrap_log() {
  printf '[nxbootstrap] %s\n' "$*"
}

nxbootstrap_set_phase() {
  NXBOOTSTRAP_CURRENT_PHASE=$1
  if [[ -n ${NXBOOTSTRAP_LOG_FD:-} ]]; then
    nxbootstrap_log "phase=$NXBOOTSTRAP_CURRENT_PHASE"
  fi
}

nxbootstrap_finish_once() {
  local status
  (( NXBOOTSTRAP_FINISHED == 0 )) || return 0
  NXBOOTSTRAP_FINISHED=1
  if declare -F pm_finish >/dev/null 2>&1; then
    if pm_finish; then
      nxbootstrap_log "pm_finish completed"
    else
      status=$?
      nxbootstrap_log "WARNING: pm_finish returned status $status"
    fi
  fi
}

nxbootstrap_tty_message() {
  local message=$1 tty=${CUR_TTY:-/dev/tty0}
  [[ ${NXBOOTSTRAP_TEST_NO_TTY:-0} == 1 ]] && return 0
  case $tty in
    /dev/console|/dev/tty[0-9]*) ;;
    *) return 0 ;;
  esac
  [[ -c $tty && ! -L $tty ]] || return 0
  printf '%s: %s\n' "${NXPORT_TITLE:-Port}" "$message" > "$tty" 2>/dev/null ||
    true
}

nxbootstrap_die() {
  NXBOOTSTRAP_STATUS=${2:-1}
  nxbootstrap_log "ERROR: $1 (phase=${NXBOOTSTRAP_CURRENT_PHASE:-unknown})"
  nxbootstrap_tty_message "$1"
  trap '' HUP INT TERM
  trap - EXIT
  nxbootstrap_cleanup
  exit "$NXBOOTSTRAP_STATUS"
}

nxbootstrap_canonical_directory() {
  (CDPATH= cd -P -- "$1" 2>/dev/null && pwd -P)
}

nxbootstrap_path_is_contained() {
  local path=$1 parent basename canonical_parent
  [[ -n ${NXBOOTSTRAP_GAME_ROOT:-} && $path == "$NXBOOTSTRAP_GAME_ROOT"/* ]] ||
    return 1
  parent=${path%/*}
  basename=${path##*/}
  [[ -n $basename ]] || return 1
  canonical_parent=$(nxbootstrap_canonical_directory "$parent") || return 1
  case $canonical_parent/$basename in
    "$NXBOOTSTRAP_GAME_ROOT"/*) return 0 ;;
    *) return 1 ;;
  esac
}

nxbootstrap_safe_regular_file() {
  local path=$1
  nxbootstrap_path_is_contained "$path" && [[ -f $path && ! -L $path ]]
}

nxbootstrap_safe_directory() {
  local path=$1
  nxbootstrap_path_is_contained "$path" && [[ -d $path && ! -L $path ]]
}

nxbootstrap_make_executable() {
  local path=$1 fd path_identity fd_identity link_count
  nxbootstrap_safe_regular_file "$path" || return 1
  if ! { exec {fd}< "$path"; } 2>/dev/null; then
    return 1
  fi
  nxbootstrap_safe_regular_file "$path" || {
    exec {fd}>&- || true
    return 1
  }
  path_identity=$(stat -L -c '%d:%i' -- "$path" 2>/dev/null) || {
    exec {fd}>&- || true
    return 1
  }
  fd_identity=$(stat -L -c '%d:%i' -- "/proc/self/fd/$fd" 2>/dev/null) || {
    exec {fd}>&- || true
    return 1
  }
  link_count=$(stat -L -c '%h' -- "/proc/self/fd/$fd" 2>/dev/null) || {
    exec {fd}>&- || true
    return 1
  }
  if [[ $path_identity != "$fd_identity" || $link_count != 1 ]] ||
     ! chmod a+x -- "/proc/self/fd/$fd" 2>/dev/null ||
     [[ ! -x /proc/self/fd/$fd ]]; then
    exec {fd}>&- || true
    return 1
  fi
  exec {fd}>&- || true
  [[ -x $path && ! -L $path ]]
}

nxbootstrap_validate_relative_path() {
  local path=$1 allow_empty=${2:-0}
  if [[ -z $path ]]; then
    (( allow_empty != 0 ))
    return
  fi
  (( ${#path} <= 512 )) || return 1
  [[ $path != /* && $path != *\\* && ! $path =~ [[:cntrl:]] ]] || return 1
  case /$path/ in
    *//*|*/./*|*/../*) return 1 ;;
  esac
}

nxbootstrap_validate_path_list() {
  local values=$1 item seen=$'\n'
  while IFS= read -r item; do
    [[ -n $item ]] || continue
    nxbootstrap_validate_relative_path "$item" || return 1
    case $seen in
      *$'\n'"$item"$'\n'*) return 1 ;;
    esac
    seen+=$item$'\n'
  done <<< "$values"
}

# Generated/audited mirror of framework/nxcompat/capabilities-v1.json.  The
# bootstrap cannot depend on Python at runtime, so it fails closed against this
# finite shell allowlist; the process-free contract gate proves exact equality.
nxbootstrap_capability_known() {
  case $1 in
    host.portmaster|host.armhf-libs|host.aarch64-libs|host.i386-libs|\
    host.session-runtime|host.short-memory|host.fuse-like-filesystem|\
    host.network-filesystem|host.fbdev|host.drm|host.drm-connected|\
    host.wayland|host.x11|audio.pulse-socket|audio.pipewire-socket|\
    audio.alsa|graphics.window|graphics.gles2|graphics.gles3|graphics.egl|\
    graphics.egl-config|graphics.drawable|graphics.etc1|graphics.etc2|\
    graphics.astc|graphics.npot-full|audio.output-open|\
    input.controller-mapping|input.controller-api|\
    input.controller-connected|input.hotplug)
      return 0
      ;;
  esac
  return 1
}

nxbootstrap_validate_named_list() {
  local kind=$1 values=$2 item seen=$'\n'
  while IFS= read -r item; do
    [[ -n $item ]] || continue
    case $kind in
      capability)
        [[ $item =~ ^(host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}$ ]] ||
          return 1
        nxbootstrap_capability_known "$item" || return 1
        ;;
      quirk)
        [[ $item =~ ^(adapter|engine|game)\.[a-z0-9][a-z0-9._-]{0,62}$ ]] ||
          return 1
        ;;
      *) return 1 ;;
    esac
    [[ .$item. != *.device.* ]] || return 1
    case $seen in
      *$'\n'"$item"$'\n'*) return 1 ;;
    esac
    seen+=$item$'\n'
  done <<< "$values"
}

nxbootstrap_capability_required() {
  local expected=$1 capability
  while IFS= read -r capability; do
    [[ $capability == "$expected" ]] && return 0
  done <<< "${NXPORT_REQUIRED_CAPABILITIES:-}"
  return 1
}

nxbootstrap_validate_config() {
  : "${NXPORT_SCHEMA_VERSION:=1}"
  case $NXPORT_SCHEMA_VERSION in
    1)
      : "${NXPORT_NXEXTRACT_VERSION:=1.2.6}"
      : "${NXPORT_REQUIRED_CAPABILITIES:=}"
      : "${NXPORT_ENABLED_QUIRKS:=}"
      : "${NXPORT_RUNTIME_REPORT:=log-and-logo}"
      ;;
    2)
      [[ ${NXPORT_NXEXTRACT_VERSION:-} == 1.2.6 ]] || {
        printf 'nxbootstrap: schema v2 requires NXExtract 1.2.6\n' >&2
        return 1
      }
      ;;
    *)
      printf 'nxbootstrap: unsupported NXPORT_SCHEMA_VERSION=%s\n' \
        "$NXPORT_SCHEMA_VERSION" >&2
      return 1
      ;;
  esac
  if [[ ! ${NXPORT_PRIVATE_LIBRARY_PATHS+x} ]]; then
    NXPORT_PRIVATE_LIBRARY_PATHS=${NXPORT_EXTRA_LIBRARY_PATHS:-}
  fi
  case ${NXPORT_RUNTIME_REPORT:-} in
    log|log-and-logo) ;;
    *)
      printf 'nxbootstrap: invalid NXPORT_RUNTIME_REPORT=%s\n' \
        "${NXPORT_RUNTIME_REPORT:-}" >&2
      return 1
      ;;
  esac
  nxbootstrap_validate_named_list capability \
    "${NXPORT_REQUIRED_CAPABILITIES:-}" || {
      printf 'nxbootstrap: invalid or duplicate required capability\n' >&2
      return 1
    }
  nxbootstrap_validate_named_list quirk "${NXPORT_ENABLED_QUIRKS:-}" || {
    printf 'nxbootstrap: invalid, duplicate or device-selected quirk\n' >&2
    return 1
  }
  case ${NXPORT_ID:-} in
    ''|*[!a-z0-9._-]*)
      printf 'nxbootstrap: invalid or missing NXPORT_ID\n' >&2
      return 1
      ;;
  esac
  (( ${#NXPORT_ID} <= 63 )) || {
    printf 'nxbootstrap: NXPORT_ID is longer than 63 characters\n' >&2
    return 1
  }
  [[ -n ${NXPORT_TITLE:-} ]] || NXPORT_TITLE=$NXPORT_ID
  [[ ! $NXPORT_TITLE =~ [[:cntrl:]] && ${#NXPORT_TITLE} -le 128 ]] || {
    printf 'nxbootstrap: NXPORT_TITLE is unsafe or longer than 128 characters\n' >&2
    return 1
  }
  [[ ${NXPORT_GAME_DIR:-} == /* && -d ${NXPORT_GAME_DIR:-} ]] || {
    printf 'nxbootstrap: NXPORT_GAME_DIR must be an existing absolute path\n' >&2
    return 1
  }
  NXBOOTSTRAP_GAME_ROOT=$(nxbootstrap_canonical_directory "$NXPORT_GAME_DIR") || {
    printf 'nxbootstrap: cannot resolve NXPORT_GAME_DIR\n' >&2
    return 1
  }
  [[ $NXBOOTSTRAP_GAME_ROOT != / ]] || {
    printf 'nxbootstrap: NXPORT_GAME_DIR cannot be the filesystem root\n' >&2
    return 1
  }
  NXPORT_GAME_DIR=$NXBOOTSTRAP_GAME_ROOT
  nxbootstrap_validate_relative_path "${NXPORT_EXECUTABLE:-}" || {
    printf 'nxbootstrap: NXPORT_EXECUTABLE must be normalized, relative and contained\n' >&2
    return 1
  }
  NXBOOTSTRAP_BIN=$NXPORT_GAME_DIR/$NXPORT_EXECUTABLE
  case ${NXPORT_ARCH:-} in
    aarch64|armv7|x86_64|i386) ;;
    *)
      printf 'nxbootstrap: unsupported NXPORT_ARCH=%s\n' "${NXPORT_ARCH:-}" >&2
      return 1
      ;;
  esac
  case ${NXPORT_ARGUMENT_MODE:=game-dir-and-passthrough} in
    none|passthrough|game-dir|game-dir-and-passthrough) ;;
    *)
      printf 'nxbootstrap: invalid NXPORT_ARGUMENT_MODE=%s\n' \
        "$NXPORT_ARGUMENT_MODE" >&2
      return 1
      ;;
  esac
  case ${NXPORT_HOME_MODE:=preserve} in
    preserve|port) ;;
    *)
      printf 'nxbootstrap: invalid NXPORT_HOME_MODE=%s\n' "$NXPORT_HOME_MODE" >&2
      return 1
      ;;
  esac
  case ${NXPORT_NXEXTRACT:=auto} in
    auto|yes|no) ;;
    *)
      printf 'nxbootstrap: invalid NXPORT_NXEXTRACT=%s\n' "$NXPORT_NXEXTRACT" >&2
      return 1
      ;;
  esac
  nxbootstrap_validate_path_list "${NXPORT_REQUIRED_FILES:-}" || {
    printf 'nxbootstrap: required_files contains an unsafe or duplicate path\n' >&2
    return 1
  }
  nxbootstrap_validate_path_list "${NXPORT_PRIVATE_LIBRARY_PATHS:-}" || {
    printf 'nxbootstrap: private_library_paths contains an unsafe or duplicate path\n' >&2
    return 1
  }
  nxbootstrap_validate_relative_path "${NXPORT_PREPARE_SCRIPT:-}" 1 || {
    printf 'nxbootstrap: prepare_script must be empty or a normalized relative path\n' >&2
    return 1
  }
}

nxbootstrap_private_tmp_root() {
  local base=${TMPDIR:-/tmp} canonical_base root uid=${UID:-$(id -u)}
  [[ $base == /* && -d $base && ! -L $base && -w $base && -x $base ]] ||
    return 1
  canonical_base=$(nxbootstrap_canonical_directory "$base") || return 1
  if [[ -n ${NXBOOTSTRAP_GAME_ROOT:-} &&
        ( $canonical_base == "$NXBOOTSTRAP_GAME_ROOT" ||
          $canonical_base == "$NXBOOTSTRAP_GAME_ROOT"/* ) ]]; then
    return 1
  fi
  base=$canonical_base
  root=$base/nxport-$uid
  if [[ ! -e $root ]]; then
    mkdir -m 0700 -- "$root" 2>/dev/null || return 1
  fi
  [[ -d $root && ! -L $root && -O $root && -w $root && -x $root ]] ||
    return 1
  chmod 0700 -- "$root" 2>/dev/null || return 1
  printf '%s\n' "$root"
}

nxbootstrap_file_link_count() {
  local path=$1 count
  count=$(stat -c '%h' -- "$path" 2>/dev/null) || return 1
  case $count in ''|*[!0-9]*) return 1 ;; esac
  printf '%s\n' "$count"
}

nxbootstrap_open_fresh_log_fd() {
  local log=$1 previous=${1%.log}.prev.log link_count fd
  local path_identity fd_identity noclobber_was_set=0
  if [[ -e $log || -L $log ]]; then
    [[ -f $log && ! -L $log && -O $log ]] || return 1
    link_count=$(nxbootstrap_file_link_count "$log") || return 1
    [[ $link_count == 1 ]] || return 1
    if [[ -e $previous || -L $previous ]]; then
      [[ ! -d $previous ]] || return 1
      rm -f -- "$previous" 2>/dev/null || return 1
    fi
    mv -- "$log" "$previous" 2>/dev/null || return 1
  fi

  [[ -o noclobber ]] && noclobber_was_set=1
  set -C
  if ! { exec {fd}> "$log"; } 2>/dev/null; then
    (( noclobber_was_set != 0 )) || set +C
    return 1
  fi
  (( noclobber_was_set != 0 )) || set +C

  if [[ ! -f $log || -L $log || ! -O $log ]] ||
     [[ $(nxbootstrap_file_link_count "$log" 2>/dev/null || true) != 1 ]]; then
    exec {fd}>&- || true
    return 1
  fi
  path_identity=$(stat -L -c '%d:%i' -- "$log" 2>/dev/null) || {
    exec {fd}>&- || true
    return 1
  }
  fd_identity=$(stat -L -c '%d:%i' -- "/proc/self/fd/$fd" 2>/dev/null) || {
    exec {fd}>&- || true
    return 1
  }
  [[ $path_identity == "$fd_identity" ]] || {
    exec {fd}>&- || true
    return 1
  }
  chmod 0600 -- "/proc/self/fd/$fd" 2>/dev/null || {
    exec {fd}>&- || true
    return 1
  }
  NXBOOTSTRAP_LOG_FD=$fd
}

nxbootstrap_open_log() {
  local log=${NXPORT_LOG_FILE:-$NXPORT_GAME_DIR/debug.log} private_root
  nxbootstrap_path_is_contained "$log" || log=$NXPORT_GAME_DIR/debug.log
  if ! nxbootstrap_path_is_contained "$log" ||
     ! nxbootstrap_open_fresh_log_fd "$log"; then
    private_root=$(nxbootstrap_private_tmp_root) || {
      printf 'nxbootstrap: cannot create a runtime log\n' >&2
      return 1
    }
    log=$private_root/${NXPORT_ID}-debug.log
    nxbootstrap_open_fresh_log_fd "$log" || {
      printf 'nxbootstrap: cannot create a runtime log\n' >&2
      return 1
    }
  fi
  NXBOOTSTRAP_LOG=$log
  exec 1>&"$NXBOOTSTRAP_LOG_FD" 2>&1
  nxbootstrap_log "run_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || printf unknown)"
  nxbootstrap_log "${NXPORT_TITLE} | nxbootstrap ${NXBOOTSTRAP_VERSION:-0.5.1}"
  nxbootstrap_log "deployment_id=${NXPORT_DEPLOYMENT_ID:-unknown} bootstrap_path=${NXPORT_BOOTSTRAP_PATH:-unknown} bootstrap_sha256=${NXPORT_BOOTSTRAP_SHA256:-unknown}"
  nxbootstrap_log "launcher_original=${NXPORT_LAUNCHER_ORIGINAL:-unknown} launcher_resolved=${NXPORT_LAUNCHER_RESOLVED:-unknown} bash=${BASH_VERSION:-unknown} pid=$$ ppid=$PPID euid=${EUID:-unknown}"
  nxbootstrap_log "game_dir=$NXPORT_GAME_DIR executable=$NXPORT_EXECUTABLE arch=$NXPORT_ARCH schema=$NXPORT_SCHEMA_VERSION"
  nxbootstrap_log "nxextract=${NXPORT_NXEXTRACT:-auto}@${NXPORT_NXEXTRACT_VERSION:-unknown} report=${NXPORT_RUNTIME_REPORT:-log}"
  nxbootstrap_log "required_capabilities=${NXPORT_REQUIRED_CAPABILITIES:-none} enabled_quirks=${NXPORT_ENABLED_QUIRKS:-none}"
}

nxbootstrap_default_portmaster_roots() {
  local game_parent xdg_data
  xdg_data=${XDG_DATA_HOME:-${HOME:-/tmp}/.local/share}
  game_parent=${NXPORT_GAME_DIR%/*}
  printf '%s\n' \
    "$game_parent/PortMaster" \
    /opt/system/Tools/PortMaster \
    /opt/tools/PortMaster \
    "$xdg_data/PortMaster" \
    "${HOME:-/tmp}/.var/app/net.retrodeck.retrodeck/data/PortMaster" \
    /storage/.config/PortMaster \
    /var/data/PortMaster \
    /PortMaster \
    /roms/tools/PortMaster \
    /roms2/tools/PortMaster \
    /roms/ports/PortMaster \
    /roms/Ports/PortMaster \
    /roms2/ports/PortMaster \
    /roms2/Ports/PortMaster \
    /storage/roms/ports/PortMaster \
    /storage/roms/Ports/PortMaster \
    /userdata/roms/ports/PortMaster \
    /userdata/roms/Ports/PortMaster \
    /userdata/system/.local/share/PortMaster \
    /mnt/ports/PortMaster \
    /mnt/Ports/PortMaster \
    /mnt/mmc/MUOS/PortMaster \
    /mnt/mmc/ports/PortMaster \
    /mnt/mmc/Ports/PortMaster \
    /mnt/mmc/roms/ports/PortMaster \
    /mnt/mmc/ROMS/ports/PortMaster \
    /mnt/SDCARD/Apps/PortMaster/PortMaster \
    /mnt/sdcard/Roms/.portmaster/PortMaster \
    /mnt/sdcard/MIYOO_EX/PortMaster \
    /mnt/sdcard/ports/PortMaster \
    /mnt/sdcard/Ports/PortMaster \
    /mnt/sdcard/roms/ports/PortMaster \
    /mnt/sdcard/ROMS/ports/PortMaster
}

nxbootstrap_install_traps() {
  trap nxbootstrap_cleanup EXIT
  trap 'nxbootstrap_on_signal 129' HUP
  trap 'nxbootstrap_on_signal 130' INT
  trap 'nxbootstrap_on_signal 143' TERM
}

nxbootstrap_load_portmaster() {
  local candidate candidate_root canonical_candidate control_mapping
  local control_status mapping_bytes mod_file roots
  roots=${NXBOOTSTRAP_PORTMASTER_ROOTS:-$(nxbootstrap_default_portmaster_roots)}
  if [[ -n ${controlfolder:-} && -f $controlfolder/control.txt &&
        ! -L $controlfolder/control.txt ]]; then
    canonical_candidate=$(nxbootstrap_canonical_directory "$controlfolder" 2>/dev/null || true)
    [[ -n $canonical_candidate ]] && candidate=$canonical_candidate
  else
    candidate=
    while IFS= read -r candidate_root; do
      [[ -n $candidate_root && -f $candidate_root/control.txt &&
         ! -L $candidate_root/control.txt ]] || continue
      canonical_candidate=$(nxbootstrap_canonical_directory "$candidate_root" 2>/dev/null || true)
      [[ -n $canonical_candidate &&
         -f $canonical_candidate/control.txt &&
         ! -L $canonical_candidate/control.txt ]] || continue
      candidate=$canonical_candidate
      break
    done <<< "$roots"
  fi
  controlfolder=${candidate:-/storage/.config/PortMaster}
  if [[ -n $candidate ]]; then
    # PortMaster control files are trusted firmware integration code. Do not
    # enable nounset: released control.txt versions read optional variables.
    # shellcheck disable=SC1090
    if ! source "$candidate/control.txt"; then
      if nxbootstrap_capability_required host.portmaster; then
        nxbootstrap_die "required capability host.portmaster unavailable: PortMaster control.txt failed to initialize"
      fi
      nxbootstrap_die "PortMaster control.txt failed to initialize"
    fi
    nxbootstrap_install_traps
    # Some platform controls are reached through a compatibility alias and
    # then publish their canonical PortMaster root. Use that canonical root
    # for mod files, runtimes and libraries after the control has loaded.
    if [[ ${controlfolder:-} == /* && -d ${controlfolder:-} ]]; then
      canonical_candidate=$(nxbootstrap_canonical_directory "$controlfolder" 2>/dev/null || true)
      [[ -n $canonical_candidate ]] && candidate=$canonical_candidate
      controlfolder=$candidate
    else
      controlfolder=$candidate
    fi
    case ${CFW_NAME:-} in
      ''|*[!A-Za-z0-9._-]*) ;;
      *)
        mod_file=$candidate/mod_${CFW_NAME}.txt
        if [[ -f $mod_file && ! -L $mod_file ]]; then
          # shellcheck disable=SC1090
          source "$mod_file" ||
            nxbootstrap_die "PortMaster platform module failed: ${mod_file##*/}"
          nxbootstrap_install_traps
        fi
        ;;
    esac
    control_status=missing
    if declare -F get_controls >/dev/null 2>&1; then
      if get_controls; then
        control_status=ok
      else
        control_status=failed
        nxbootstrap_log "WARNING: PortMaster get_controls returned a failure"
      fi
    fi
    # Released control.txt/funcs.txt versions install their own EXIT trap.
    # Reassert our lifecycle contract after sourcing them; pm_finish remains
    # the single PortMaster-owned cleanup entry point called by our handler.
    nxbootstrap_install_traps
    export NXCOMPAT_PORTMASTER_DIR=$candidate
    control_mapping=${sdl_controllerconfig:-}
    mapping_bytes=${#control_mapping}
    nxbootstrap_log "PortMaster loaded (root=$candidate cfw=${CFW_NAME:-unknown} funcs=${PM_FUNCS_VERSION:-legacy} device_info=${DEVICE_INFO_VERSION:-legacy})"
    nxbootstrap_log "controls=$control_status mapping_bytes=$mapping_bytes database=${SDL_GAMECONTROLLERCONFIG_FILE:-unknown}"
  else
    unset NXCOMPAT_PORTMASTER_DIR
    if nxbootstrap_capability_required host.portmaster; then
      nxbootstrap_die "required capability host.portmaster unavailable: PortMaster control.txt not found"
    fi
    nxbootstrap_log "PortMaster control.txt not found; standalone mode"
  fi
  : "${CUR_TTY:=/dev/tty0}"
}

nxbootstrap_discover_session_runtime() {
  local candidate uid=${UID:-$(id -u)}
  if [[ ${XDG_RUNTIME_DIR:-} == /* && -d ${XDG_RUNTIME_DIR:-} &&
        -w ${XDG_RUNTIME_DIR:-} && -x ${XDG_RUNTIME_DIR:-} ]]; then
    return 0
  fi
  for candidate in "/run/$uid-runtime-dir" "/var/run/$uid-runtime-dir" \
                   "/run/user/$uid" "/var/run/user/$uid" \
                   /run/0-runtime-dir /var/run/0-runtime-dir; do
    if [[ -d $candidate && -w $candidate && -x $candidate ]]; then
      export XDG_RUNTIME_DIR=$candidate
      nxbootstrap_log "existing writable session runtime discovered: $candidate"
      return 0
    fi
  done
  unset XDG_RUNTIME_DIR
  nxbootstrap_log "no writable firmware session runtime discovered"
}

nxbootstrap_capture_and_filter_inherited_libraries() {
  local directory canonical_directory old_path=${LD_LIBRARY_PATH:-}
  (( NXBOOTSTRAP_INHERITED_LD_CAPTURED == 0 )) || return 0
  NXBOOTSTRAP_INHERITED_LD_CAPTURED=1
  NXBOOTSTRAP_INHERITED_LD_LIBRARY_PATH=$old_path
  NXBOOTSTRAP_LIBRARY_PATH=
  IFS=: read -r -a nxbootstrap_early_dirs <<< "$old_path"
  for directory in "${nxbootstrap_early_dirs[@]}"; do
    [[ $directory == /* && -d $directory ]] || continue
    canonical_directory=$(nxbootstrap_canonical_directory "$directory") || continue
    if [[ $canonical_directory == "$NXPORT_GAME_DIR" ||
          $canonical_directory == "$NXPORT_GAME_DIR"/* ]]; then
      continue
    fi
    nxbootstrap_add_library_dir "$canonical_directory"
  done
  if [[ -n $NXBOOTSTRAP_LIBRARY_PATH ]]; then
    export LD_LIBRARY_PATH=$NXBOOTSTRAP_LIBRARY_PATH
  else
    unset LD_LIBRARY_PATH
  fi
}

nxbootstrap_process_starttime() {
  local pid=$1 stat rest
  case $pid in ''|*[!0-9]*) return 1 ;; esac
  IFS= read -r stat 2>/dev/null \
    < "$NXBOOTSTRAP_PROC_ROOT/$pid/stat" || return 1
  rest=${stat##*) }
  set -- $rest
  (( $# >= 20 )) || return 1
  printf '%s\n' "${20}"
}

nxbootstrap_owner_alive() {
  local pid=$1 expected=$2 actual
  [[ -d $NXBOOTSTRAP_PROC_ROOT/$pid ]] || return 1
  actual=$(nxbootstrap_process_starttime "$pid") || return 1
  [[ $actual == "$expected" ]]
}

nxbootstrap_prepare_lock_paths() {
  local base canonical_base root uid=${UID:-$(id -u)}
  if [[ -n ${NXBOOTSTRAP_LOCK_ROOT:-} ]]; then
    base=$NXBOOTSTRAP_LOCK_ROOT
    [[ $base == /* && -d $base && ! -L $base && -O $base &&
       -w $base && -x $base ]] ||
      return 1
  elif [[ ${XDG_RUNTIME_DIR:-} == /* && -d ${XDG_RUNTIME_DIR:-} &&
          ! -L ${XDG_RUNTIME_DIR:-} && -O ${XDG_RUNTIME_DIR:-} &&
          -w ${XDG_RUNTIME_DIR:-} && -x ${XDG_RUNTIME_DIR:-} ]]; then
    base=$XDG_RUNTIME_DIR
  else
    root=$(nxbootstrap_private_tmp_root) || return 1
  fi
  if [[ -n ${base:-} ]]; then
    canonical_base=$(nxbootstrap_canonical_directory "$base") || return 1
    if [[ -n ${NXBOOTSTRAP_GAME_ROOT:-} &&
          ( $canonical_base == "$NXBOOTSTRAP_GAME_ROOT" ||
            $canonical_base == "$NXBOOTSTRAP_GAME_ROOT"/* ) ]]; then
      return 1
    fi
    base=$canonical_base
    root=$base/.nxbootstrap-$uid
    if [[ ! -e $root ]]; then
      (umask 077; mkdir -- "$root") 2>/dev/null || return 1
    fi
    [[ -d $root && ! -L $root && -O $root && -w $root && -x $root ]] ||
      return 1
    chmod 0700 -- "$root" 2>/dev/null || return 1
  fi
  # The identity is deliberately global per port ID, not per ROM path. Two
  # copies in /roms and /roms2 are still the same game and must never own the
  # framebuffer/audio simultaneously.
  NXBOOTSTRAP_LOCK_BASE=$root/nxport-${NXPORT_ID}
}

nxbootstrap_try_flock() {
  local lock_file=$NXBOOTSTRAP_LOCK_BASE.flock status link_count
  local path_identity fd_identity
  [[ ! -e $lock_file || ( -f $lock_file && ! -L $lock_file ) ]] || return 1
  # Append mode avoids truncating even if an unexpected hard link exists.
  if ! { exec 9>> "$lock_file"; } 2>/dev/null; then
    return 1
  fi
  [[ -f $lock_file && ! -L $lock_file && -O $lock_file ]] || {
    exec 9>&- || true
    return 1
  }
  link_count=$(stat -L -c '%h' -- /proc/self/fd/9 2>/dev/null) || {
    exec 9>&- || true
    return 1
  }
  [[ $link_count == 1 ]] || {
    exec 9>&- || true
    return 1
  }
  path_identity=$(stat -L -c '%d:%i' -- "$lock_file" 2>/dev/null) || {
    exec 9>&- || true
    return 1
  }
  fd_identity=$(stat -L -c '%d:%i' -- /proc/self/fd/9 2>/dev/null) || {
    exec 9>&- || true
    return 1
  }
  [[ $path_identity == "$fd_identity" ]] || {
    exec 9>&- || true
    return 1
  }
  # BusyBox flock does not consistently implement util-linux -E. Closing fd 9
  # releases the lock, so the portable nonblocking form is sufficient.
  flock -n 9 2>/dev/null
  status=$?
  if (( status != 0 )); then
    exec 9>&- || true
    return 1
  fi
  NXBOOTSTRAP_LOCK_KIND=flock
  NXBOOTSTRAP_LOCK_PATH=$lock_file
  return 0
}

nxbootstrap_create_mkdir_lock() {
  local lock_dir=$1 self_start
  (umask 077; mkdir -- "$lock_dir") 2>/dev/null || return 1
  self_start=$(nxbootstrap_process_starttime "$$") || {
    rmdir -- "$lock_dir" 2>/dev/null || true
    return 1
  }
  if ! (umask 077; printf '%s %s\n' "$$" "$self_start" > "$lock_dir/owner"); then
    rm -f -- "$lock_dir/owner" 2>/dev/null || true
    rmdir -- "$lock_dir" 2>/dev/null || true
    return 1
  fi
  NXBOOTSTRAP_LOCK_KIND=mkdir
  NXBOOTSTRAP_LOCK_PATH=$lock_dir
  NXBOOTSTRAP_LOCK_OWNER="$$ $self_start"
  return 0
}

nxbootstrap_try_mkdir_lock() {
  local lock_dir=$NXBOOTSTRAP_LOCK_BASE.lockdir
  [[ ! -L $lock_dir ]] || return 1
  nxbootstrap_create_mkdir_lock "$lock_dir" && return 0
  # Never reclaim an existing mkdir lock automatically. POSIX shell has no
  # compare-and-swap for a pathname: a timeout/reaper can rename a live creator
  # before it writes owner and allow two games to own Mali/audio. Normal
  # INT/TERM/EXIT removes this directory; an uncatchable SIGKILL fails closed
  # until the private runtime tmp is cleared or an operator audits it.
  return 1
}

nxbootstrap_try_lock() {
  if [[ ${NXBOOTSTRAP_TEST_FORCE_MKDIR_LOCK:-0} == 1 ]]; then
    nxbootstrap_try_mkdir_lock
    return
  fi
  if command -v flock >/dev/null 2>&1; then
    nxbootstrap_try_flock
    return
  fi
  nxbootstrap_try_mkdir_lock
}

nxbootstrap_release_lock() {
  local owner
  case ${NXBOOTSTRAP_LOCK_KIND:-} in
    flock)
      flock -u 9 2>/dev/null || true
      exec 9>&- || true
      ;;
    mkdir)
      if [[ -d ${NXBOOTSTRAP_LOCK_PATH:-} &&
            ! -L ${NXBOOTSTRAP_LOCK_PATH:-} &&
            -f ${NXBOOTSTRAP_LOCK_PATH:-}/owner &&
            ! -L ${NXBOOTSTRAP_LOCK_PATH:-}/owner ]]; then
        owner=$(<"$NXBOOTSTRAP_LOCK_PATH/owner")
        if [[ $owner == "$NXBOOTSTRAP_LOCK_OWNER" ]]; then
          rm -f -- "$NXBOOTSTRAP_LOCK_PATH/owner" 2>/dev/null || true
          rmdir -- "$NXBOOTSTRAP_LOCK_PATH" 2>/dev/null || true
        fi
      fi
      ;;
  esac
  NXBOOTSTRAP_LOCK_KIND=
}

nxbootstrap_send_signal() {
  local signal=$1 pid=$2
  case $signal in TERM|KILL) ;; *) return 1 ;; esac
  case $pid in ''|*[!0-9]*) return 1 ;; esac
  [[ $pid != "$$" && $pid != "${PPID:-}" ]] || return 1
  builtin kill -"$signal" "$pid" 2>/dev/null
}

nxbootstrap_acquire_lock() {
  nxbootstrap_prepare_lock_paths || return 1
  if nxbootstrap_try_lock; then
    return 0
  fi
  # A contended lock proves only that another owner exists.  It does not grant
  # authority to scan the host /proc tree and signal processes selected by
  # ambient cwd, comm or environment data.  Fail closed until the exact lock
  # owner can be verified by PID + starttime from lock metadata.
  nxbootstrap_log "another instance owns the port lock; refusing to launch"
  return 1
}

nxbootstrap_read_uint_le() {
  local file=$1 offset=$2 size=$3 bytes byte value=0 multiplier=1
  bytes=$(LC_ALL=C od -An -v -j "$offset" -N "$size" -t u1 "$file" \
    2>/dev/null) || return 1
  set -- $bytes
  (( $# == size )) || return 1
  for byte in "$@"; do
    case $byte in ''|*[!0-9]*) return 1 ;; esac
    (( byte >= 0 && byte <= 255 )) || return 1
    value=$((value + byte * multiplier))
    multiplier=$((multiplier * 256))
  done
  printf '%s\n' "$value"
}

nxbootstrap_read_uint64_bounded() {
  local file=$1 offset=$2 low high
  low=$(nxbootstrap_read_uint_le "$file" "$offset" 4) || return 1
  high=$(nxbootstrap_read_uint_le "$file" "$((offset + 4))" 4) || return 1
  [[ $high == 0 ]] || return 1
  printf '%s\n' "$low"
}

nxbootstrap_validate_elf_contract() {
  local file=$1 expected_arch=$2 expected_interpreter=$3 ident file_size
  local elf_class elf_data elf_version elf_type machine expected_class
  local expected_machine phoff phentsize phnum ph_end index entry p_type
  local p_offset p_filesz interp_count=0 interpreter
  for utility in od dd wc; do
    command -v "$utility" >/dev/null 2>&1 || return 1
  done
  ident=$(LC_ALL=C od -An -v -j 0 -N 16 -t u1 "$file" 2>/dev/null) ||
    return 1
  set -- $ident
  (( $# == 16 )) || return 1
  [[ $1 == 127 && $2 == 69 && $3 == 76 && $4 == 70 ]] || return 1
  elf_class=$5
  elf_data=$6
  elf_version=$7
  [[ $elf_data == 1 && $elf_version == 1 ]] || return 1
  elf_type=$(nxbootstrap_read_uint_le "$file" 16 2) || return 1
  machine=$(nxbootstrap_read_uint_le "$file" 18 2) || return 1
  [[ $elf_type == 2 || $elf_type == 3 ]] || return 1
  case $expected_arch in
    aarch64) expected_class=2; expected_machine=183 ;;
    armv7) expected_class=1; expected_machine=40 ;;
    *) return 1 ;;
  esac
  [[ $elf_class == "$expected_class" && $machine == "$expected_machine" ]] ||
    return 1
  file_size=$(LC_ALL=C wc -c < "$file" 2>/dev/null) || return 1
  file_size=${file_size//[[:space:]]/}
  case $file_size in ''|*[!0-9]*) return 1 ;; esac
  if [[ $elf_class == 1 ]]; then
    phoff=$(nxbootstrap_read_uint_le "$file" 28 4) || return 1
    phentsize=$(nxbootstrap_read_uint_le "$file" 42 2) || return 1
    phnum=$(nxbootstrap_read_uint_le "$file" 44 2) || return 1
    [[ $phentsize == 32 ]] || return 1
  else
    phoff=$(nxbootstrap_read_uint64_bounded "$file" 32) || return 1
    phentsize=$(nxbootstrap_read_uint_le "$file" 54 2) || return 1
    phnum=$(nxbootstrap_read_uint_le "$file" 56 2) || return 1
    [[ $phentsize == 56 ]] || return 1
  fi
  (( phnum >= 1 && phnum <= 256 && phoff <= file_size )) || return 1
  ph_end=$((phoff + phentsize * phnum))
  (( ph_end >= phoff && ph_end <= file_size )) || return 1

  for (( index = 0; index < phnum; ++index )); do
    entry=$((phoff + index * phentsize))
    p_type=$(nxbootstrap_read_uint_le "$file" "$entry" 4) || return 1
    [[ $p_type == 3 ]] || continue
    interp_count=$((interp_count + 1))
    (( interp_count == 1 )) || return 1
    if [[ $elf_class == 1 ]]; then
      p_offset=$(nxbootstrap_read_uint_le "$file" "$((entry + 4))" 4) ||
        return 1
      p_filesz=$(nxbootstrap_read_uint_le "$file" "$((entry + 16))" 4) ||
        return 1
    else
      p_offset=$(nxbootstrap_read_uint64_bounded "$file" "$((entry + 8))") ||
        return 1
      p_filesz=$(nxbootstrap_read_uint64_bounded "$file" "$((entry + 32))") ||
        return 1
    fi
    (( p_filesz >= 2 && p_filesz <= 512 && p_offset <= file_size &&
       p_offset + p_filesz >= p_offset &&
       p_offset + p_filesz <= file_size )) || return 1
    interpreter=
    IFS= read -r -d '' interpreter < <(
      LC_ALL=C dd if="$file" bs=1 skip="$p_offset" count="$p_filesz" 2>/dev/null
    ) || return 1
    (( ${#interpreter} + 1 == p_filesz )) || return 1
    [[ $interpreter == "$expected_interpreter" ]] || return 1
  done
  [[ $interp_count == 1 ]]
}

nxbootstrap_check_arch() {
  local machine expected_interpreter
  machine=$(uname -m 2>/dev/null || printf unknown)
  case $NXPORT_ARCH in
    aarch64)
      [[ $machine == aarch64 || $machine == arm64 ]] ||
        nxbootstrap_die "this port needs an AArch64 userland (found $machine)"
      expected_interpreter=/lib/ld-linux-aarch64.so.1
      ;;
    armv7)
      export PORT_32BIT=Y
      [[ $machine == armv7* || $machine == armv8l || $machine == aarch64 ||
         $machine == arm64 ]] ||
        nxbootstrap_die "this port needs an ARMHF-capable userland (found $machine)"
      expected_interpreter=/lib/ld-linux-armhf.so.3
      ;;
    x86_64)
      [[ $machine == x86_64 || ${NXBOOTSTRAP_TEST_NATIVE_ARCH:-0} == 1 ]] ||
        nxbootstrap_die "this port needs an x86_64 userland (found $machine)"
      expected_interpreter=/lib64/ld-linux-x86-64.so.2
      ;;
    i386)
      [[ $machine == i?86 || $machine == x86_64 ]] ||
        nxbootstrap_die "this port needs an i386-capable userland (found $machine)"
      expected_interpreter=/lib/ld-linux.so.2
      ;;
  esac
  : "${NXPORT_INTERPRETER:=$expected_interpreter}"
  [[ $NXPORT_INTERPRETER == "$expected_interpreter" ]] ||
    nxbootstrap_die "non-canonical PT_INTERP for $NXPORT_ARCH: $NXPORT_INTERPRETER"
  [[ -f $NXPORT_INTERPRETER && -r $NXPORT_INTERPRETER &&
     -x $NXPORT_INTERPRETER ]] ||
    nxbootstrap_die "required dynamic linker is not a usable file: $NXPORT_INTERPRETER"
  case $NXPORT_ARCH in
    aarch64|armv7)
      nxbootstrap_validate_elf_contract "$NXBOOTSTRAP_BIN" "$NXPORT_ARCH" \
        "$expected_interpreter" ||
        nxbootstrap_die "runtime ELF architecture/PT_INTERP contract is invalid"
      ;;
  esac
}

nxbootstrap_run_extractor() {
  local mode=${NXPORT_NXEXTRACT:-auto}
  local recipe=$NXPORT_GAME_DIR/extractor.json
  local runner=$NXPORT_GAME_DIR/nxextract/run-extractor.sh
  local core=$NXPORT_GAME_DIR/nxextract/nxextract.py
  local helper=$NXPORT_GAME_DIR/nxextract/nxextract-runtime-env.sh
  case $mode in
    no) return 0 ;;
    yes) ;;
    auto)
      [[ -f $recipe || -f $runner ]] || return 0
      ;;
    *) nxbootstrap_die "invalid NXPORT_NXEXTRACT=$mode" ;;
  esac
  [[ -s $recipe ]] && nxbootstrap_safe_regular_file "$recipe" &&
  [[ -s $runner ]] && nxbootstrap_safe_regular_file "$runner" &&
  [[ -s $core ]] && nxbootstrap_safe_regular_file "$core" &&
  [[ -s $helper ]] && nxbootstrap_safe_regular_file "$helper" ||
    nxbootstrap_die "incomplete NXExtract integration"
  command -v python3 >/dev/null 2>&1 ||
    nxbootstrap_die "python3 is required for owner-data extraction"
  nxbootstrap_log "running NXExtract in an isolated foreground phase"
  NXEXTRACT_GAME_DIR=$NXPORT_GAME_DIR \
  NXEXTRACT_FIRMWARE_LIBRARY_PATH=$(nxbootstrap_firmware_library_path) \
    nxbootstrap_run_phase bash "$runner" ||
      nxbootstrap_die "owner-data extraction failed; see nxextract.log"
}

nxbootstrap_run_prepare() {
  local relative=${NXPORT_PREPARE_SCRIPT:-}
  [[ -n $relative ]] || return 0
  case $relative in
    /*|*'..'*) nxbootstrap_die "prepare script must stay inside the port" ;;
  esac
  nxbootstrap_safe_regular_file "$NXPORT_GAME_DIR/$relative" ||
    nxbootstrap_die "prepare script is missing or unsafe: $relative"
  nxbootstrap_log "running the declared prepare phase"
  NXPORT_GAME_DIR=$NXPORT_GAME_DIR \
    nxbootstrap_run_phase bash "$NXPORT_GAME_DIR/$relative" ||
    nxbootstrap_die "prepare phase failed"
}

nxbootstrap_run_phase() {
  local status
  "$@" &
  NXBOOTSTRAP_PHASE_PID=$!
  NXBOOTSTRAP_PHASE_STARTTIME=$(
    nxbootstrap_process_starttime "$NXBOOTSTRAP_PHASE_PID" 2>/dev/null || true
  )
  wait "$NXBOOTSTRAP_PHASE_PID"
  status=$?
  NXBOOTSTRAP_PHASE_PID=
  NXBOOTSTRAP_PHASE_STARTTIME=
  return "$status"
}

nxbootstrap_check_required_files() {
  local relative
  while IFS= read -r relative; do
    [[ -n $relative ]] || continue
    case $relative in
      /*|*'..'*) nxbootstrap_die "required path escapes the port: $relative" ;;
    esac
    [[ -s $NXPORT_GAME_DIR/$relative ]] &&
      nxbootstrap_safe_regular_file "$NXPORT_GAME_DIR/$relative" ||
      nxbootstrap_die "required file is missing: $relative"
  done <<< "${NXPORT_REQUIRED_FILES:-}"
}

nxbootstrap_firmware_library_path() {
  case $NXPORT_ARCH in
    armv7)
      printf '%s' \
        '/usr/local/lib32:/usr/lib32:/lib32:' \
        '/usr/local/lib/arm-linux-gnueabihf:' \
        '/usr/lib/arm-linux-gnueabihf:/lib/arm-linux-gnueabihf:' \
        '/usr/local/lib:/usr/lib:/lib'
      ;;
    aarch64)
      printf '%s' \
        '/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:' \
        '/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:' \
        '/usr/lib:/lib'
      ;;
    i386)
      printf '%s' \
        '/usr/local/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu:' \
        '/lib/i386-linux-gnu:/usr/lib32:/lib32:/usr/lib:/lib'
      ;;
    *) printf '%s' '/usr/local/lib:/usr/lib64:/lib64:/usr/lib:/lib' ;;
  esac
}

nxbootstrap_add_library_dir() {
  local directory=$1
  [[ $directory == /* && -d $directory ]] || return 0
  case :${NXBOOTSTRAP_LIBRARY_PATH:-}: in
    *:"$directory":*) ;;
    *) NXBOOTSTRAP_LIBRARY_PATH=${NXBOOTSTRAP_LIBRARY_PATH:+$NXBOOTSTRAP_LIBRARY_PATH:}$directory ;;
  esac
}

nxbootstrap_validate_private_library_dir() {
  local directory=$1 pattern candidate
  nxbootstrap_safe_directory "$directory" ||
    nxbootstrap_die "private library directory is missing or escapes the port: $directory"
  # Firmware owns the display stack. Shipping one inside a generic private
  # LD_LIBRARY_PATH silently replaces Mali/Panfrost/KMS behavior by device.
  for pattern in 'libEGL.so*' 'libGLES*.so*' 'libGL.so*' 'libOpenGL.so*' \
                 'libgbm.so*' 'libdrm.so*' 'libmali.so*' \
                 'libSDL.so*' 'libSDL-1.2.so*' 'libSDL2.so*' \
                 'libSDL2-2.0.so*'; do
    for candidate in "$directory"/$pattern; do
      [[ -e $candidate || -L $candidate ]] || continue
      nxbootstrap_die "private library directory contains a forbidden graphics provider: ${candidate##*/}"
    done
  done
}

nxbootstrap_build_runtime_environment() {
  nxbootstrap_build_library_path 1
  if [[ $NXPORT_HOME_MODE == port ]]; then
    export HOME=$NXPORT_GAME_DIR
  fi
  nxbootstrap_log "runtime-private paths prepared; SDL backends remain automatic"
}

nxbootstrap_build_library_path() {
  local include_private=$1 directory canonical_directory pm_arch
  local old_path=${NXBOOTSTRAP_INHERITED_LD_LIBRARY_PATH:-${LD_LIBRARY_PATH:-}}
  NXBOOTSTRAP_LIBRARY_PATH=
  case $NXPORT_ARCH in
    armv7) pm_arch=armhf ;;
    aarch64) pm_arch=aarch64 ;;
    i386) pm_arch=i386 ;;
    x86_64) pm_arch=x86_64 ;;
  esac
  if (( include_private != 0 )); then
    while IFS= read -r directory; do
      [[ -n $directory ]] || continue
      case $directory in
        /*|*'..'*) nxbootstrap_die "library path escapes the port: $directory" ;;
      esac
      directory=$NXPORT_GAME_DIR/$directory
      nxbootstrap_validate_private_library_dir "$directory"
      nxbootstrap_add_library_dir "$directory"
    done <<< "${NXPORT_PRIVATE_LIBRARY_PATHS:-}"
  fi
  nxbootstrap_add_library_dir "$controlfolder/libs"
  nxbootstrap_add_library_dir "$controlfolder/libs.$pm_arch"
  if [[ $pm_arch != "$NXPORT_ARCH" ]]; then
    nxbootstrap_add_library_dir "$controlfolder/libs.${NXPORT_ARCH}"
  fi
  case $NXPORT_ARCH in
    armv7)
      for directory in /usr/local/lib32 /usr/lib32 /lib32 \
        /usr/local/lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf \
        /lib/arm-linux-gnueabihf; do
        nxbootstrap_add_library_dir "$directory"
      done
      ;;
    aarch64)
      for directory in /usr/local/lib/aarch64-linux-gnu /usr/local/lib \
        /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu; do
        nxbootstrap_add_library_dir "$directory"
      done
      ;;
    i386)
      for directory in /usr/local/lib/i386-linux-gnu /usr/lib/i386-linux-gnu \
        /lib/i386-linux-gnu /usr/local/lib32 /usr/lib32 /lib32; do
        nxbootstrap_add_library_dir "$directory"
      done
      ;;
    x86_64)
      for directory in /usr/local/lib/x86_64-linux-gnu /usr/local/lib64 \
        /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 \
        /usr/local/lib; do
        nxbootstrap_add_library_dir "$directory"
      done
      ;;
  esac
  IFS=: read -r -a nxbootstrap_old_dirs <<< "$old_path"
  for directory in "${nxbootstrap_old_dirs[@]}"; do
    [[ $directory == /* && -d $directory ]] || continue
    canonical_directory=$(nxbootstrap_canonical_directory "$directory") ||
      continue
    if [[ $canonical_directory == "$NXPORT_GAME_DIR" ||
          $canonical_directory == "$NXPORT_GAME_DIR"/* ]]; then
      continue
    fi
    nxbootstrap_add_library_dir "$canonical_directory"
  done
  nxbootstrap_add_library_dir /usr/lib
  nxbootstrap_add_library_dir /lib
  export LD_LIBRARY_PATH=$NXBOOTSTRAP_LIBRARY_PATH
}

nxbootstrap_build_host_environment() {
  (( NXBOOTSTRAP_INHERITED_LD_CAPTURED != 0 )) ||
    nxbootstrap_capture_and_filter_inherited_libraries
  nxbootstrap_build_library_path 0
  export NXCOMPAT_PORT_ID=$NXPORT_ID
  export NXCOMPAT_GAME_DIR=$NXPORT_GAME_DIR
  export NXCOMPAT_REQUIRED_CAPABILITIES=${NXPORT_REQUIRED_CAPABILITIES:-}
  export NXCOMPAT_ENABLED_QUIRKS=${NXPORT_ENABLED_QUIRKS:-}
  export NXCOMPAT_RUNTIME_REPORT=${NXPORT_RUNTIME_REPORT:-log-and-logo}
  if [[ -n ${sdl_controllerconfig:-} ]]; then
    export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig
  fi
  nxbootstrap_log "firmware/PortMaster host paths prepared"
}

nxbootstrap_platform_prepare() {
  local dialog_pipe status
  (( NXBOOTSTRAP_PLATFORM_PREPARED == 0 )) || return 0
  if declare -F pm_platform_helper >/dev/null 2>&1; then
    if pm_platform_helper "$NXBOOTSTRAP_BIN"; then
      nxbootstrap_log "pm_platform_helper completed before setup UI"
    else
      status=$?
      # Released launchers do not abort when an optional platform helper (for
      # example the ROCKNIX Sway fullscreen hint) returns non-zero.
      nxbootstrap_log "WARNING: pm_platform_helper returned status $status; continuing"
    fi
  fi
  # A platform module may replace PortMaster's default helper and accidentally
  # omit PortMasterDialogExit.  In that state the helper reports success while
  # pugwash still owns the display; a game can then run normally (including
  # audio and input) behind a black screen.  Close only the dialog proven by
  # PortMaster's own live pipe, and only through PortMaster's own API.  This is
  # state-based rather than a CFW-name workaround, and never scans or signals a
  # foreign process.
  dialog_pipe=${PM_PIPE:-}
  if [[ -n $dialog_pipe &&
        ( -e $dialog_pipe || -L $dialog_pipe ) ]]; then
    if [[ ! -p $dialog_pipe || -L $dialog_pipe ]]; then
      nxbootstrap_die "PortMaster dialog handoff failed: PM_PIPE is not a live non-symlink FIFO"
    fi
    if ! declare -F PortMasterDialogExit >/dev/null 2>&1; then
      nxbootstrap_die "PortMaster dialog handoff failed: close API unavailable while PM_PIPE is active"
    fi
    if PortMasterDialogExit; then
      :
    else
      status=$?
      nxbootstrap_die "PortMaster dialog handoff failed: close API returned status $status"
    fi
    if [[ -e $dialog_pipe || -L $dialog_pipe ]]; then
      nxbootstrap_die "PortMaster dialog handoff failed: PM_PIPE remained after close request"
    fi
    nxbootstrap_log "PortMaster dialog closed after platform helper"
  fi
  NXBOOTSTRAP_PLATFORM_PREPARED=1
}

nxbootstrap_stop_tracked_process() {
  local pid=$1 start=$2 attempt
  local attempts=${NXBOOTSTRAP_TERM_ATTEMPTS:-10}
  local delay=${NXBOOTSTRAP_TERM_DELAY:-1}
  [[ -n $pid && -n $start ]] || return 0
  nxbootstrap_owner_alive "$pid" "$start" || return 0
  case $attempts in ''|*[!0-9]*) attempts=10 ;; esac
  (( attempts >= 1 && attempts <= 30 )) || attempts=10
  case $delay in ''|*[!0-9]*) delay=1 ;; esac
  (( delay <= 5 )) || delay=1
  # Authority is limited to the direct child created by this bootstrap.  A
  # cwd/comm/cmdline/procfs scan cannot prove ownership and must never feed a
  # signal.  A phase or engine that creates more processes must supervise them
  # inside that phase/adapter and remain in the foreground until they exit.
  nxbootstrap_send_signal TERM "$pid" || true
  for (( attempt = 0; attempt < attempts; ++attempt )); do
    nxbootstrap_owner_alive "$pid" "$start" || return 0
    sleep "$delay"
  done
  nxbootstrap_owner_alive "$pid" "$start" || return 0
  nxbootstrap_send_signal KILL "$pid" || true
}

nxbootstrap_stop_child() {
  nxbootstrap_stop_tracked_process "${NXBOOTSTRAP_CHILD_PID:-}" \
    "${NXBOOTSTRAP_CHILD_STARTTIME:-}"
}

nxbootstrap_stop_phase() {
  nxbootstrap_stop_tracked_process "${NXBOOTSTRAP_PHASE_PID:-}" \
    "${NXBOOTSTRAP_PHASE_STARTTIME:-}"
  if [[ -n ${NXBOOTSTRAP_PHASE_PID:-} ]]; then
    wait "$NXBOOTSTRAP_PHASE_PID" 2>/dev/null || true
  fi
  NXBOOTSTRAP_PHASE_PID=
  NXBOOTSTRAP_PHASE_STARTTIME=
}

nxbootstrap_on_signal() {
  NXBOOTSTRAP_STATUS=$1
  trap '' HUP INT TERM
  trap - EXIT
  nxbootstrap_cleanup
  exit "$NXBOOTSTRAP_STATUS"
}

nxbootstrap_cleanup() {
  (( NXBOOTSTRAP_CLEANED == 0 && NXBOOTSTRAP_CLEANING == 0 )) || return 0
  NXBOOTSTRAP_CLEANING=1
  nxbootstrap_log "cleanup phase=${NXBOOTSTRAP_CURRENT_PHASE:-unknown} status=${NXBOOTSTRAP_STATUS:-unknown}"
  # Cleanup is deliberately restricted to the phase and child roots launched
  # by this exact nxbootstrap process.  Never perform a host-wide /proc sweep
  # from EXIT: a false positive can terminate the desktop/session supervisor.
  nxbootstrap_stop_phase
  nxbootstrap_stop_child
  nxbootstrap_finish_once
  nxbootstrap_release_lock
  NXBOOTSTRAP_CLEANED=1
}

nxbootstrap_launch() {
  local status
  nxbootstrap_platform_prepare
  case $NXPORT_ARGUMENT_MODE in
    none) "$NXBOOTSTRAP_BIN" & ;;
    passthrough) "$NXBOOTSTRAP_BIN" "$@" & ;;
    game-dir) "$NXBOOTSTRAP_BIN" "$NXPORT_GAME_DIR" & ;;
    game-dir-and-passthrough)
      "$NXBOOTSTRAP_BIN" "$NXPORT_GAME_DIR" "$@" &
      ;;
  esac
  NXBOOTSTRAP_CHILD_PID=$!
  NXBOOTSTRAP_CHILD_STARTTIME=$(
    nxbootstrap_process_starttime "$NXBOOTSTRAP_CHILD_PID" 2>/dev/null || true
  )
  wait "$NXBOOTSTRAP_CHILD_PID"
  status=$?
  NXBOOTSTRAP_CHILD_PID=
  NXBOOTSTRAP_CHILD_STARTTIME=
  NXBOOTSTRAP_STATUS=$status
  nxbootstrap_log "game exited with status $status"
  nxbootstrap_finish_once
  nxbootstrap_release_lock
  NXBOOTSTRAP_CLEANED=1
  trap - HUP INT TERM EXIT
  return "$status"
}

nxbootstrap_main() {
  local status
  nxbootstrap_set_phase validate-config
  nxbootstrap_validate_config || exit 2
  nxbootstrap_set_phase capture-host-environment
  nxbootstrap_capture_and_filter_inherited_libraries
  nxbootstrap_open_log || exit 2
  nxbootstrap_set_phase runtime-log-open
  nxbootstrap_install_traps
  cd -- "$NXPORT_GAME_DIR" || nxbootstrap_die "cannot enter the port directory"
  if [[ $NXPORT_ARCH == armv7 ]]; then
    # The visible wrapper must also contain the literal for muOS static scan.
    export PORT_32BIT=Y
  fi
  nxbootstrap_set_phase portmaster-load
  nxbootstrap_load_portmaster
  nxbootstrap_set_phase session-runtime-discovery
  nxbootstrap_discover_session_runtime
  nxbootstrap_set_phase runtime-file-validation
  [[ -s $NXBOOTSTRAP_BIN ]] && nxbootstrap_safe_regular_file "$NXBOOTSTRAP_BIN" ||
    nxbootstrap_die "runtime is missing or unsafe: $NXPORT_EXECUTABLE"
  nxbootstrap_make_executable "$NXBOOTSTRAP_BIN" ||
    nxbootstrap_die "runtime is not executable: $NXPORT_EXECUTABLE"
  nxbootstrap_set_phase elf-abi-validation
  nxbootstrap_check_arch
  nxbootstrap_set_phase instance-lock
  nxbootstrap_acquire_lock || nxbootstrap_die "another instance still owns the port"
  nxbootstrap_set_phase host-environment
  nxbootstrap_build_host_environment
  nxbootstrap_set_phase display-handoff
  nxbootstrap_platform_prepare
  nxbootstrap_set_phase owner-data-extraction
  nxbootstrap_run_extractor
  nxbootstrap_set_phase prepare
  nxbootstrap_run_prepare
  nxbootstrap_set_phase required-files
  nxbootstrap_check_required_files
  nxbootstrap_set_phase runtime-environment
  nxbootstrap_build_runtime_environment
  nxbootstrap_set_phase game-launch
  nxbootstrap_launch "$@"
  status=$?
  nxbootstrap_set_phase complete
  return "$status"
}
