#!/usr/bin/env bash
# Universal runtime launcher.
#
# ARMHF behavior is intentionally kept equivalent to the current public
# launcher.  The Box32 path is reachable only for the exact X5M/NextOS
# hardware signature validated in July 2026.

RUN_SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd -P) ||
  exit 1
GAMEDIR="${ASM2_GAMEDIR:-$RUN_SCRIPT_DIR}"
CONTROLFOLDER="${ASM2_CONTROLFOLDER:-/roms/ports/PortMaster}"

ARM_NEXTOS_BIN="$GAMEDIR/bin/asm2-nextos-armhf"
ARM_PORTMASTER_BIN="$GAMEDIR/bin/asm2-portmaster-armhf"
X5_GUEST_BIN="$GAMEDIR/asm2_127_x86_box32"
X5_BOX32_BIN="$GAMEDIR/runtime/x5m/box64"
X5_NATIVE_LIB_DIR="$GAMEDIR/runtime/x5m/native"
X5_SDL2_COMPAT="$X5_NATIVE_LIB_DIR/libSDL2-2.0.so.0"
X5_GUEST_LIBRARY="$GAMEDIR/libtasm2-x86.so"
OWNER_RUNTIME_APK="$GAMEDIR/gamefiles/base.apk"
X5_ENV_HELPER="${ASM2_X5_ENV_HELPER:-$RUN_SCRIPT_DIR/x5m-runtime-env.sh}"

# Deterministic low-glibc rebuilds of the scoped source/runtime profile.
# BUILD-PROVENANCE.json separates the historical physical proof from the
# exact-byte rebuild and QEMU smoke evidence for this release.
X5_BOX32_SHA256=d73aa019eefd4e553acda8ef7a126f88101cd1cdf46502e4060c192412c3f4dc
X5_SDL2_COMPAT_SHA256=798051928c553ee27fde9e2d555be4a9fe4ddcefbbb72f59252e427cbcb0d452
X5_GUEST_BIN_SHA256=201c2ef029451a005f047090db05f8f0d9f61b43eccf5309b936ba4306a7b110
X5_GUEST_LIBRARY_SHA256=d146d38574c19a105df8a46e523f626c06004c8f71bbeed5cf77e919dbf81a12
ARM32_ONLY_RUNTIME_APK_SHA256=95ffd25a6623e731e80156df82066e4a2b1475466adb337389b93aeed0f1ea71

launcher_error() {
  printf 'ASM2: %s\n' "$*" >&2
  exit 1
}

[ -f "$X5_ENV_HELPER" ] && [ ! -L "$X5_ENV_HELPER" ] ||
  launcher_error "X5M process-scope helper is missing"
# shellcheck source=x5m-runtime-env.sh
source "$X5_ENV_HELPER"
declare -F asm2_x5m_run_extractor >/dev/null 2>&1 &&
  declare -F asm2_x5m_run_game >/dev/null 2>&1 ||
  launcher_error "X5M process-scope helper is incomplete"

process_starttime() {
  local stat_line stat_fields
  local -a fields

  IFS= read -r stat_line < "/proc/$1/stat" 2>/dev/null || return 1
  stat_fields=${stat_line#*) }
  read -r -a fields <<< "$stat_fields"
  [ "${#fields[@]}" -ge 20 ] || return 1
  case "${fields[19]}" in
    ''|*[!0-9]*) return 1 ;;
  esac
  printf '%s\n' "${fields[19]}"
}

LOCK_FILE=
LOCK_DIR=
LOCK_KIND=
LOCK_START=

lock_cleanup() {
  local owner_pid owner_start

  if [ "$LOCK_KIND" = mkdir ] && [ -r "$LOCK_DIR/owner" ]; then
    read -r owner_pid owner_start < "$LOCK_DIR/owner" || return
    if [ "$owner_pid" = "$$" ] && [ "$owner_start" = "$LOCK_START" ]; then
      rm -f -- "$LOCK_DIR/owner"
      rmdir -- "$LOCK_DIR" 2>/dev/null || true
    fi
  fi
}

acquire_launch_lock() {
  local lock_pid lock_start live_start stale_dir

  LOCK_FILE="$GAMEDIR/.asm2-launch.lock"
  LOCK_DIR="$GAMEDIR/.asm2-launch.lock.d"
  LOCK_START=$(process_starttime "$$")
  case "$LOCK_START" in
    ''|*[!0-9]*) launcher_error "could not identify the launcher process" ;;
  esac

  if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE" ||
      launcher_error "could not open the single-instance lock"
    flock -n 9 ||
      launcher_error "another The Amazing Spider-Man 2 launcher is active"
    LOCK_KIND=flock
  else
    while ! mkdir "$LOCK_DIR" 2>/dev/null; do
      [ -r "$LOCK_DIR/owner" ] ||
        launcher_error "the existing launcher lock has no readable owner"
      read -r lock_pid lock_start < "$LOCK_DIR/owner" ||
        launcher_error "the existing launcher lock is invalid"
      case "$lock_pid:$lock_start" in
        *[!0-9:]*|:*|*:)
          launcher_error "the existing launcher lock is invalid"
          ;;
      esac

      live_start=$(process_starttime "$lock_pid")
      [ "$live_start" != "$lock_start" ] ||
        launcher_error "another The Amazing Spider-Man 2 launcher is active"

      stale_dir="${LOCK_DIR}.stale.$$.$LOCK_START"
      if mv -- "$LOCK_DIR" "$stale_dir" 2>/dev/null; then
        rm -f -- "$stale_dir/owner"
        rmdir -- "$stale_dir" 2>/dev/null || true
      fi
    done

    printf '%s %s\n' "$$" "$LOCK_START" > "$LOCK_DIR/owner" || {
      rm -f -- "$LOCK_DIR/owner"
      rmdir -- "$LOCK_DIR" 2>/dev/null || true
      launcher_error "could not record the launcher lock owner"
    }
    LOCK_KIND=mkdir
  fi

  trap lock_cleanup EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
}

matching_game_pids() {
  local process pid comm command_line executable working_directory matched

  for process in /proc/[0-9]*; do
    [ -d "$process" ] || continue
    pid=${process##*/}
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "${PPID:-}" ] && continue

    comm=
    IFS= read -r comm < "$process/comm" 2>/dev/null || true
    command_line=$(LC_ALL=C command tr '\000' ' ' \
      < "$process/cmdline" 2>/dev/null || true)
    executable=$(command readlink "$process/exe" 2>/dev/null || true)
    working_directory=$(command readlink "$process/cwd" 2>/dev/null || true)
    matched=0

    # readlink reports an exact " (deleted)" suffix when a running ELF was
    # replaced.  Both the live and deleted identities are deliberate.
    case "$executable" in
      "$ARM_NEXTOS_BIN"|"$ARM_NEXTOS_BIN (deleted)"|\
      "$ARM_PORTMASTER_BIN"|"$ARM_PORTMASTER_BIN (deleted)"|\
      "$X5_GUEST_BIN"|"$X5_GUEST_BIN (deleted)"|\
      "$X5_BOX32_BIN"|"$X5_BOX32_BIN (deleted)")
        matched=1
        ;;
    esac

    if [ "$matched" -eq 0 ]; then
      case "$working_directory" in
        "$GAMEDIR"|"$GAMEDIR (deleted)")
          case "$comm" in
            asm2-nextos-ar*|asm2-portmaster*|asm2_127_x86_*|\
            box64|box32)
              matched=1
              ;;
          esac
          ;;
      esac
    fi

    if [ "$matched" -eq 0 ]; then
      case "$working_directory:$command_line" in
        "$GAMEDIR:"*"$ARM_NEXTOS_BIN"*|\
        "$GAMEDIR:"*"$ARM_PORTMASTER_BIN"*|\
        "$GAMEDIR:"*"$X5_GUEST_BIN"*|\
        "$GAMEDIR:"*"$X5_BOX32_BIN"*|\
        "$GAMEDIR:"*" ./bin/asm2-nextos-armhf "*|\
        "$GAMEDIR:"*" ./bin/asm2-portmaster-armhf "*|\
        "$GAMEDIR:"*" ./asm2_127_x86_box32 "*)
          matched=1
          ;;
      esac
    fi

    [ "$matched" -eq 0 ] || printf '%s\n' "$pid"
  done
}

stop_existing_game() {
  local old_pids pid attempt remaining

  old_pids=$(matching_game_pids)
  if [ -n "$old_pids" ]; then
    for pid in $old_pids; do
      printf '[launcher] stopping old game instance pid=%s\n' "$pid"
      kill "$pid" 2>/dev/null || true
    done

    attempt=0
    remaining=$(matching_game_pids)
    while [ -n "$remaining" ] && [ "$attempt" -lt 20 ]; do
      sleep 0.5
      attempt=$((attempt + 1))
      remaining=$(matching_game_pids)
    done

    if [ -n "$remaining" ]; then
      for pid in $remaining; do
        printf '[launcher] forcing old game instance to stop pid=%s\n' "$pid"
        kill -9 "$pid" 2>/dev/null || true
      done
      sleep 1
    fi
  fi

  remaining=$(matching_game_pids)
  [ -z "$remaining" ] ||
    launcher_error "an older game instance could not be stopped: $remaining"
}

u16le_at() {
  local raw
  local -a bytes

  raw=$(LC_ALL=C command od -An -v -t u1 -j "$2" -N 2 "$1" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a bytes <<< "$raw"
  [ "${#bytes[@]}" -eq 2 ] || return 1
  printf '%s\n' $((bytes[0] + (bytes[1] << 8)))
}

u32le_at() {
  local raw
  local -a bytes

  raw=$(LC_ALL=C command od -An -v -t u1 -j "$2" -N 4 "$1" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a bytes <<< "$raw"
  [ "${#bytes[@]}" -eq 4 ] || return 1
  printf '%s\n' \
    $((bytes[0] + (bytes[1] << 8) + (bytes[2] << 16) + (bytes[3] << 24)))
}

u64le_at() {
  local raw
  local -a bytes

  raw=$(LC_ALL=C command od -An -v -t u1 -j "$2" -N 8 "$1" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a bytes <<< "$raw"
  [ "${#bytes[@]}" -eq 8 ] || return 1
  # ELF offsets and sizes used here are bounded by the local file size.  A
  # negative shell integer therefore cannot be a valid result.
  printf '%s\n' \
    $((bytes[0] + (bytes[1] << 8) + (bytes[2] << 16) + \
       (bytes[3] << 24) + (bytes[4] << 32) + (bytes[5] << 40) + \
       (bytes[6] << 48) + (bytes[7] << 56)))
}

is_elf_machine() {
  local file=$1 expected_class=$2 expected_machine=$3 allowed_types=$4
  local raw elf_type elf_machine
  local -a header

  [ -f "$file" ] || return 1
  raw=$(LC_ALL=C command od -An -v -t u1 -N 20 "$file" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a header <<< "$raw"
  [ "${#header[@]}" -eq 20 ] || return 1
  [ "${header[0]}" -eq 127 ] &&
    [ "${header[1]}" -eq 69 ] &&
    [ "${header[2]}" -eq 76 ] &&
    [ "${header[3]}" -eq 70 ] &&
    [ "${header[4]}" -eq "$expected_class" ] &&
    [ "${header[5]}" -eq 1 ] &&
    [ "${header[6]}" -eq 1 ] || return 1

  elf_type=$((header[16] + (header[17] << 8)))
  elf_machine=$((header[18] + (header[19] << 8)))
  [ "$elf_machine" -eq "$expected_machine" ] || return 1
  case ",$allowed_types," in
    *",$elf_type,"*) return 0 ;;
  esac
  return 1
}

is_armhf_elf() {
  local flags

  is_elf_machine "$1" 1 40 "2,3" || return 1
  flags=$(u32le_at "$1" 36) || return 1
  [ $((flags & 0x400)) -ne 0 ]
}

is_i386_executable() {
  is_elf_machine "$1" 1 3 "2,3"
}

is_i386_shared_object() {
  is_elf_machine "$1" 1 3 "3"
}

is_aarch64_executable() {
  is_elf_machine "$1" 2 183 "2,3"
}

is_aarch64_shared_object() {
  is_elf_machine "$1" 2 183 "3"
}

elf_interpreter() {
  local file=$1 raw elf_class file_size phoff phentsize phnum
  local index entry p_type p_offset p_filesz interpreter
  local -a header

  raw=$(LC_ALL=C command od -An -v -t u1 -N 6 "$file" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a header <<< "$raw"
  [ "${#header[@]}" -eq 6 ] || return 1
  [ "${header[0]}" -eq 127 ] &&
    [ "${header[1]}" -eq 69 ] &&
    [ "${header[2]}" -eq 76 ] &&
    [ "${header[3]}" -eq 70 ] &&
    [ "${header[5]}" -eq 1 ] || return 1
  elf_class=${header[4]}

  file_size=$(command wc -c < "$file" 2>/dev/null) || return 1
  case "$elf_class" in
    1)
      phoff=$(u32le_at "$file" 28) || return 1
      phentsize=$(u16le_at "$file" 42) || return 1
      phnum=$(u16le_at "$file" 44) || return 1
      [ "$phentsize" -ge 32 ] || return 1
      ;;
    2)
      phoff=$(u64le_at "$file" 32) || return 1
      phentsize=$(u16le_at "$file" 54) || return 1
      phnum=$(u16le_at "$file" 56) || return 1
      [ "$phentsize" -ge 56 ] || return 1
      ;;
    *) return 1 ;;
  esac
  [ "$phoff" -ge 0 ] && [ "$phnum" -gt 0 ] && [ "$phnum" -le 128 ] ||
    return 1

  index=0
  while [ "$index" -lt "$phnum" ]; do
    entry=$((phoff + index * phentsize))
    [ "$entry" -ge 0 ] && [ $((entry + phentsize)) -le "$file_size" ] ||
      return 1
    p_type=$(u32le_at "$file" "$entry") || return 1
    if [ "$p_type" -eq 3 ]; then
      if [ "$elf_class" -eq 1 ]; then
        p_offset=$(u32le_at "$file" $((entry + 4))) || return 1
        p_filesz=$(u32le_at "$file" $((entry + 16))) || return 1
      else
        p_offset=$(u64le_at "$file" $((entry + 8))) || return 1
        p_filesz=$(u64le_at "$file" $((entry + 32))) || return 1
      fi
      [ "$p_offset" -ge 0 ] &&
        [ "$p_filesz" -gt 1 ] &&
        [ "$p_filesz" -le 4096 ] &&
        [ $((p_offset + p_filesz)) -le "$file_size" ] || return 1
      interpreter=$(command dd if="$file" bs=1 skip="$p_offset" \
        count="$p_filesz" 2>/dev/null | LC_ALL=C command tr -d '\000') ||
        return 1
      case "$interpreter" in
        /*)
          printf '%s\n' "$interpreter"
          return 0
          ;;
      esac
      return 1
    fi
    index=$((index + 1))
  done
  return 1
}

platform_path() {
  local root=${1%/}
  printf '%s%s\n' "$root" "$2"
}

is_nextos() {
  local root=${1:-/} label os_release health

  label="${ASM2_CFW_NAME:-} ${ASM2_OS_NAME:-}"
  case "$label" in
    *[Nn][Ee][Xx][Tt][Oo][Ss]*|\
    *[Rr][Ee][Tt][Rr][Oo]*[Ee][Ll][Ii][Tt][Ee]*)
      return 0
      ;;
  esac

  health=$(platform_path "$root" /usr/bin/nextos-pulse-health)
  [ -x "$health" ] && return 0
  os_release=$(platform_path "$root" /etc/os-release)
  [ -r "$os_release" ] &&
    LC_ALL=C command grep -Eiq \
      'nextos|retro[[:space:]_-]*elite' "$os_release"
}

file_contains_x5m_signature() {
  local path=$1

  [ -r "$path" ] || return 1
  LC_ALL=C command tr '\000' '\n' < "$path" 2>/dev/null |
    LC_ALL=C command grep -Eiq \
      '^(amlogic,[[:space:]]*s7d|amlogic,[[:space:]]*s905x5m|s905x5m)$'
}

is_tested_x5m_nextos() {
  local root=${1:-/} machine=${2:-} marker

  case "$machine" in
    aarch64|arm64) ;;
    *) return 1 ;;
  esac
  is_nextos "$root" || return 1

  # The shipped X5M DT identifies the SoC as the exact NUL-terminated string
  # "amlogic, s7d".  More explicit future kernels may expose s905x5m.
  for marker in \
    "$(platform_path "$root" /proc/device-tree/compatible)" \
    "$(platform_path "$root" /sys/firmware/devicetree/base/compatible)" \
    "$(platform_path "$root" /sys/devices/soc0/soc_id)"; do
    file_contains_x5m_signature "$marker" && return 0
  done
  return 1
}

verify_sha256() {
  local path=$1 expected=$2 label=$3 actual

  command -v sha256sum >/dev/null 2>&1 ||
    launcher_error "sha256sum is required for the X5M integrity preflight"
  actual=$(LC_ALL=C command sha256sum -- "$path" 2>/dev/null)
  actual=${actual%%[[:space:]]*}
  [ "$actual" = "$expected" ] ||
    launcher_error "$label failed the package integrity check"
}

PLATFORM_KIND=
BIN=
PLATFORM_HELPER_TARGET=

select_platform() {
  local root=${1:-/} machine=${2:-}

  if is_tested_x5m_nextos "$root" "$machine"; then
    PLATFORM_KIND=x5m-box32
    BIN=$X5_GUEST_BIN
    PLATFORM_HELPER_TARGET=$X5_BOX32_BIN
    printf '[launcher] selected tested NextOS S905X5M Box32 path\n'
  elif is_nextos "$root"; then
    PLATFORM_KIND=armhf
    BIN=$ARM_NEXTOS_BIN
    PLATFORM_HELPER_TARGET=$BIN
    printf '[launcher] selected current-NextOS ARMHF build\n'
  else
    PLATFORM_KIND=armhf
    BIN=$ARM_PORTMASTER_BIN
    PLATFORM_HELPER_TARGET=$BIN
    printf '[launcher] selected low-glibc PortMaster ARMHF build\n'
  fi
}

preflight_armhf() {
  local interpreter runtime_report runtime_status directory ld_extra

  [ -f "$BIN" ] && [ ! -L "$BIN" ] ||
    launcher_error "compatible loader is missing or linked: $BIN"
  chmod +x "$BIN" 2>/dev/null || true
  [ -x "$BIN" ] ||
    launcher_error "compatible loader is not executable: $BIN"
  is_armhf_elf "$BIN" ||
    launcher_error "the selected loader is not a valid Linux ARMHF executable"
  interpreter=$(elf_interpreter "$BIN") ||
    launcher_error "could not validate the loader's ARMHF interpreter"

  ld_extra=
  for directory in \
    /usr/local/lib/arm-linux-gnueabihf \
    /usr/local/lib32 \
    /usr/local/lib \
    /usr/lib32 \
    /usr/lib/arm-linux-gnueabihf \
    /lib/arm-linux-gnueabihf \
    "$CONTROLFOLDER/libs" \
    "$CONTROLFOLDER/libs.armhf"; do
    [ -d "$directory" ] && ld_extra="$ld_extra:$directory"
  done
  if [ -n "$ld_extra" ]; then
    export LD_LIBRARY_PATH="${ld_extra#:}:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  else
    export LD_LIBRARY_PATH="$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi

  [ -x "$interpreter" ] ||
    launcher_error \
      "ARMHF runtime is missing: $interpreter (enable this firmware's ARMHF multilib)"
  "$interpreter" --verify "$BIN" >/dev/null 2>&1 ||
    launcher_error "the kernel/runtime cannot execute the selected ARMHF loader"
  runtime_report=$(LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    "$interpreter" --list "$BIN" 2>&1)
  runtime_status=$?
  if [ "$runtime_status" -ne 0 ]; then
    printf '%s\n' "$runtime_report" >&2
    launcher_error "the ARMHF runtime could not resolve the loader dependencies"
  fi
  case "$runtime_report" in
    *"not found"*)
      printf '%s\n' "$runtime_report" |
        command awk '/not found/' >&2
      launcher_error "one or more ARMHF runtime libraries are missing"
      ;;
  esac
}

preflight_x5m_public_runtime() {
  local box_interpreter runtime_report runtime_status system_sdl3

  for artifact in "$X5_BOX32_BIN" "$X5_GUEST_BIN" "$X5_SDL2_COMPAT"; do
    [ -f "$artifact" ] && [ ! -L "$artifact" ] ||
      launcher_error "X5M runtime component is missing or linked: $artifact"
  done
  chmod +x "$X5_BOX32_BIN" "$X5_GUEST_BIN" 2>/dev/null || true
  [ -x "$X5_BOX32_BIN" ] && [ -x "$X5_GUEST_BIN" ] ||
    launcher_error "X5M Box32 or i386 loader is not executable"

  is_aarch64_executable "$X5_BOX32_BIN" ||
    launcher_error "the X5M Box32 host is not a valid AArch64 ELF"
  is_i386_executable "$X5_GUEST_BIN" ||
    launcher_error "the ASM2 Box32 loader is not a valid i386 ELF"
  is_aarch64_shared_object "$X5_SDL2_COMPAT" ||
    launcher_error "the X5M SDL2 compatibility library is not AArch64"

  verify_sha256 "$X5_BOX32_BIN" "$X5_BOX32_SHA256" "X5M Box32"
  verify_sha256 \
    "$X5_GUEST_BIN" "$X5_GUEST_BIN_SHA256" "ASM2 i386 loader"
  verify_sha256 \
    "$X5_SDL2_COMPAT" "$X5_SDL2_COMPAT_SHA256" "X5M SDL2 compatibility library"

  [ -c /dev/dri/card0 ] ||
    launcher_error "the tested X5M DRM card is unavailable: /dev/dri/card0"
  [ -c /dev/mali0 ] ||
    launcher_error "the tested X5M Mali device is unavailable: /dev/mali0"
  system_sdl3=/usr/lib/libSDL3.so.0
  [ -f "$system_sdl3" ] ||
    launcher_error "the NextOS SDL3 runtime is unavailable: $system_sdl3"
  is_aarch64_shared_object "$system_sdl3" ||
    launcher_error "the NextOS SDL3 runtime is not a valid AArch64 ELF"

  box_interpreter=$(elf_interpreter "$X5_BOX32_BIN") ||
    launcher_error "could not validate the X5M Box32 interpreter"
  [ -x "$box_interpreter" ] ||
    launcher_error "the X5M AArch64 runtime is missing: $box_interpreter"
  "$box_interpreter" --verify "$X5_BOX32_BIN" >/dev/null 2>&1 ||
    launcher_error "the X5M AArch64 runtime cannot execute Box32"
  runtime_report=$("$box_interpreter" --list "$X5_BOX32_BIN" 2>&1)
  runtime_status=$?
  if [ "$runtime_status" -ne 0 ]; then
    printf '%s\n' "$runtime_report" >&2
    launcher_error "the X5M AArch64 runtime could not resolve Box32"
  fi
  case "$runtime_report" in
    *"not found"*)
      printf '%s\n' "$runtime_report" |
        command awk '/not found/' >&2
      launcher_error "one or more X5M Box32 dependencies are missing"
      ;;
  esac
}

configure_arm_video_runtime() {
  local wayland_socket= wayland_candidate= runtime_directory socket
  local runtime_fallback

  export SDL_VIDEO_FULLSCREEN_DESKTOP=1

  if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    case "$WAYLAND_DISPLAY" in
      /*) wayland_candidate=$WAYLAND_DISPLAY ;;
      *) wayland_candidate="${XDG_RUNTIME_DIR:-}/$WAYLAND_DISPLAY" ;;
    esac
    if [ -S "$wayland_candidate" ]; then
      wayland_socket=$wayland_candidate
    else
      printf '[launcher] ignoring WAYLAND_DISPLAY without a socket: %s\n' \
        "$WAYLAND_DISPLAY"
      unset WAYLAND_DISPLAY
    fi
  fi

  if [ -z "$wayland_socket" ]; then
    for runtime_directory in \
      "${XDG_RUNTIME_DIR:-}" \
      "/run/user/$(id -u)" \
      "/var/run/user/$(id -u)" \
      /run/user/0 \
      /var/run/user/0; do
      [ -n "$runtime_directory" ] && [ -d "$runtime_directory" ] || continue
      for socket in "$runtime_directory"/wayland-*; do
        if [ -S "$socket" ]; then
          export XDG_RUNTIME_DIR="$runtime_directory"
          export WAYLAND_DISPLAY="${socket##*/}"
          wayland_socket=$socket
          break 2
        fi
      done
    done
  fi

  if [ -z "$wayland_socket" ] &&
     { [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ] ||
       [ ! -w "$XDG_RUNTIME_DIR" ]; }; then
    for runtime_directory in \
      "/run/user/$(id -u)" \
      "/var/run/user/$(id -u)" \
      /run/user/0 \
      /var/run/user/0; do
      if [ -d "$runtime_directory" ] && [ -w "$runtime_directory" ]; then
        export XDG_RUNTIME_DIR="$runtime_directory"
        break
      fi
    done
  fi

  if [ -z "$wayland_socket" ] &&
     { [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ] ||
       [ ! -w "$XDG_RUNTIME_DIR" ]; }; then
    runtime_fallback="/tmp/asm2-runtime-$(id -u)"
    mkdir -p "$runtime_fallback" &&
      chmod 700 "$runtime_fallback" &&
      export XDG_RUNTIME_DIR="$runtime_fallback"
  fi
  [ -n "$wayland_socket" ] || unset WAYLAND_DISPLAY

  if [ -r /sys/class/display/mode ]; then
    export ASM2_FORCE_OPAQUE="${ASM2_FORCE_OPAQUE:-1}"
  fi
}

configure_x5m_runtime() {
  export BOX64_NOPERSONA32BITS=1
  export BOX64_DYNAREC=1
  export BOX64_DYNAREC_BIGBLOCK=0
  export BOX64_DYNAREC_SAFEFLAGS=2
  export SDL_VIDEODRIVER=kmsdrm
  export SDL_VIDEO_DRIVER=kmsdrm
  export SDL_KMSDRM_DEVICE_INDEX=0
  export SDL_KMSDRM_REQUIRE_DRM_MASTER=1
  unset WAYLAND_DISPLAY
}

configure_arm_audio_runtime() {
  local root=${1:-/} directory

  [ "$PLATFORM_KIND" = armhf ] || return 0
  export PORT_32BIT=Y

  # Some AArch64 CFWs route their ALSA default through PipeWire. Their ARMHF
  # SDL process must load the matching 32-bit ALSA, PipeWire and SPA modules;
  # otherwise the host silently searches the AArch64 module tree.
  for directory in \
    "$(platform_path "$root" /usr/lib32)" \
    "$(platform_path "$root" /usr/lib/arm-linux-gnueabihf)" \
    "$(platform_path "$root" /usr/local/lib/arm-linux-gnueabihf)"; do
    if [ -d "$directory/pipewire-0.3" ]; then
      export PIPEWIRE_MODULE_DIR="$directory/pipewire-0.3"
    fi
    if [ -d "$directory/spa-0.2" ]; then
      export SPA_PLUGIN_DIR="$directory/spa-0.2"
    fi
    if [ -f "$directory/alsa-lib/libasound_module_pcm_pipewire.so" ]; then
      export ALSA_PLUGIN_DIR="$directory/alsa-lib"
    fi
  done

  if [ -n "${PIPEWIRE_MODULE_DIR:-}${SPA_PLUGIN_DIR:-}${ALSA_PLUGIN_DIR:-}" ]; then
    printf '[launcher] ARMHF audio modules pipewire=%s spa=%s alsa=%s\n' \
      "${PIPEWIRE_MODULE_DIR:-system}" \
      "${SPA_PLUGIN_DIR:-system}" \
      "${ALSA_PLUGIN_DIR:-system}"
  fi
}

configure_common_runtime() {
  local pulse_socket memory_kib controller_db

  for pulse_socket in /var/run/pulse/native /run/pulse/native; do
    if [ -S "$pulse_socket" ]; then
      export PULSE_SERVER="unix:$pulse_socket"
      break
    fi
  done

  memory_kib=$(command awk '/MemTotal/{print $2; exit}' \
    /proc/meminfo 2>/dev/null || true)
  case "$memory_kib" in
    ''|*[!0-9]*) memory_kib=0 ;;
  esac
  if [ "$memory_kib" -gt 0 ] && [ "$memory_kib" -lt 1200000 ]; then
    export MALLOC_ARENA_MAX=2
    export MALLOC_TRIM_THRESHOLD_=131072
    export MALLOC_MMAP_THRESHOLD_=65536
    printf '[launcher] low-memory allocator profile (%s MiB)\n' \
      "$((memory_kib / 1024))"
  fi

  for controller_db in \
    "$CONTROLFOLDER/gamecontrollerdb.txt" \
    /opt/system/Tools/PortMaster/gamecontrollerdb.txt \
    /roms/ports/PortMaster/gamecontrollerdb.txt \
    /usr/share/SDL2/gamecontrollerdb.txt; do
    if [ -f "$controller_db" ]; then
      export SDL_GAMECONTROLLERCONFIG_FILE="$controller_db"
      break
    fi
  done
}

prepare_owner_data() {
  local extractor_status

  export ASM2_RUN=1
  if [ -x "$GAMEDIR/run-extractor.sh" ] &&
     [ -f "$GAMEDIR/extractor.json" ]; then
    if [ "$PLATFORM_KIND" = x5m-box32 ]; then
      # The native AArch64 NXExtract UI is proven on the X5M with the
      # firmware SDL2/KMSDRM stack.  Do not let the game-only sdl2-compat
      # interpose here: it cannot acquire the ES handoff before Box32 starts.
      asm2_x5m_run_extractor "$GAMEDIR" "$CONTROLFOLDER"
      extractor_status=$?
    else
      NXEXTRACT_GAME_DIR="$GAMEDIR" \
        "$GAMEDIR/run-extractor.sh"
      extractor_status=$?
    fi
    if [ "$extractor_status" -ne 0 ]; then
      printf 'ASM2: owner-data preparation failed (%d)\n' \
        "$extractor_status" >&2
      return "$extractor_status"
    fi
  fi
}

plant_first_accept_seed() {
  # The first-run modals are touch-driven and their layout varies per panel
  # aspect, so handhelds without a touchscreen can get stuck on the legal
  # notice. Planting the game's own post-acceptance records makes the game
  # skip that flow deterministically; the loader touch recovery remains as a
  # fallback. Existing owner data is never overwritten.
  local seed_dir="$GAMEDIR/seed/first-accept"
  local data_dir
  local seed_file target planted=0
  data_dir="$GAMEDIR/gamefiles/Android/data"
  data_dir="$data_dir/com.gameloft.android.ANMP.GloftASHM/files"

  [ -d "$seed_dir" ] || return 0
  mkdir -p -- "$data_dir" 2>/dev/null || return 0
  for seed_file in "$seed_dir"/ud_Control.sav "$seed_dir"/ud_OObjects.sav; do
    [ -f "$seed_file" ] && [ ! -L "$seed_file" ] || continue
    target="$data_dir/$(basename -- "$seed_file")"
    [ ! -e "$target" ] || continue
    cp -- "$seed_file" "$target" 2>/dev/null || continue
    planted=$((planted + 1))
  done
  [ "$planted" -eq 0 ] ||
    printf '[launcher] first-accept seed planted (%d file(s))\n' "$planted"
  return 0
}

validate_x5m_owner_data() {
  local owner_apk_hash

  [ -f "$OWNER_RUNTIME_APK" ] && [ ! -L "$OWNER_RUNTIME_APK" ] ||
    launcher_error "NXExtract did not create a validated runtime base.apk"
  owner_apk_hash=$(LC_ALL=C command sha256sum -- "$OWNER_RUNTIME_APK" 2>/dev/null)
  owner_apk_hash=${owner_apk_hash%%[[:space:]]*}
  [ "$owner_apk_hash" != "$ARM32_ONLY_RUNTIME_APK_SHA256" ] ||
    launcher_error \
      "this validated 1.2.8d source is ARM32/multilib-only; the X5M route requires a supported APK containing the x86 game library"

  [ -f "$X5_GUEST_LIBRARY" ] && [ ! -L "$X5_GUEST_LIBRARY" ] ||
    launcher_error \
      "NXExtract did not create the required i386 game library: libtasm2-x86.so"
  is_i386_shared_object "$X5_GUEST_LIBRARY" ||
    launcher_error "libtasm2-x86.so is not the validated i386 shared object"
  verify_sha256 \
    "$X5_GUEST_LIBRARY" "$X5_GUEST_LIBRARY_SHA256" "owner i386 game library"
}

run_game_once() {
  if [ "$PLATFORM_KIND" = x5m-box32 ]; then
    # Keep packaged sdl2-compat private to Box32. NXExtract uses the
    # firmware SDL2/KMSDRM scope implemented by the same public helper.
    asm2_x5m_run_game "$GAMEDIR" "$X5_NATIVE_LIB_DIR" "$X5_BOX32_BIN"
  else
    "$BIN" "$GAMEDIR"
  fi
}

run_game_foreground() {
  local restart_count=0 game_status

  while :; do
    run_game_once
    game_status=$?

    if [ "$game_status" -eq 75 ] && [ "$restart_count" -eq 0 ]; then
      restart_count=1
      printf '[launcher] RestartGame requested; restarting once in foreground\n'
      sleep 1
      stop_existing_game
      continue
    fi

    if [ "$game_status" -eq 75 ]; then
      printf '[launcher] repeated RestartGame ignored after one restart\n'
    fi
    return "$game_status"
  done
}

asm2_launcher_main() {
  local required_tool machine status

  cd "$GAMEDIR" || launcher_error "could not enter the game directory"
  acquire_launch_lock
  : > "$GAMEDIR/debug.log"
  exec > "$GAMEDIR/debug.log" 2>&1
  printf '=== The Amazing Spider-Man 2 1.2.7d/1.2.8d | port 1.1.9 | %s ===\n' \
    "$(date -Is 2>/dev/null || date)"

  for required_tool in od dd tr wc grep awk readlink; do
    command -v "$required_tool" >/dev/null 2>&1 ||
      launcher_error "required validation tool is missing: $required_tool"
  done

  stop_existing_game
  machine=$(uname -m 2>/dev/null) ||
    launcher_error "could not identify the host architecture"
  select_platform / "$machine"

  if [ "$PLATFORM_KIND" = x5m-box32 ]; then
    preflight_x5m_public_runtime
    configure_x5m_runtime
  else
    preflight_armhf
    configure_arm_video_runtime
    configure_arm_audio_runtime /
  fi
  configure_common_runtime

  # Extraction deliberately precedes validation or execution of the owner
  # i386 library.  A public clean package never contains libtasm2-x86.so.
  prepare_owner_data || return $?
  plant_first_accept_seed
  if [ "$PLATFORM_KIND" = x5m-box32 ]; then
    validate_x5m_owner_data
  fi
  if [ "${ASM2_EXTRACTOR_ONLY:-0}" = 1 ]; then
    printf '[launcher] extractor-only validation completed; game skipped\n'
    return 0
  fi

  # The 100%-working Horizon Chase port on this X5M leaves frontend/DRM
  # ownership to the EmulationStation PortMaster launch flow and keeps its
  # complete runtime launcher in the foreground. Passing raw Box32 to a
  # platform helper would lose the guest arguments and scoped environment.
  if [ "$PLATFORM_KIND" = x5m-box32 ]; then
    printf '[launcher] using Horizon-style X5M foreground lifecycle\n'
  elif command -v pm_platform_helper >/dev/null 2>&1; then
    pm_platform_helper "$PLATFORM_HELPER_TARGET" >/dev/null ||
      launcher_error "PortMaster could not prepare the frontend lifecycle"
  fi

  run_game_foreground
  status=$?
  return "$status"
}

if [ "${ASM2_LAUNCHER_LIBRARY_ONLY:-0}" != 1 ]; then
  asm2_launcher_main "$@"
  exit $?
fi
