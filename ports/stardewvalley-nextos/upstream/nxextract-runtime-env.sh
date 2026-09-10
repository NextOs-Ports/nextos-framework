#!/usr/bin/env bash
# Process-scope boundary for the native NXExtract UI.
#
# Native AArch64 helpers must resolve SDL2/KMSDRM/Wayland from the running
# firmware first. This mirrors the RG-DS-proven Horizon Chase setup path and
# prevents a later compatibility library from interposing SDL2. The ordering
# is deliberately confined to the NXExtract child; the game gets its own scope
# later in the launcher.
#
# Adaptation policy: select libraries by ABI, preserve every valid SDL backend
# chosen by the firmware (Wayland, KMSDRM, X11 or autodetect), and keep device
# exceptions capability-scoped. The only backend reset is the already-tested
# S905X5M/NextOS path, where SDL autodetection is the known working behavior.

sdv_platform_path() {
  local root=${1:-/} absolute_path=$2

  root=${root%/}
  [ -n "$root" ] || root=/
  if [ "$root" = / ]; then
    printf '%s\n' "$absolute_path"
  else
    printf '%s%s\n' "$root" "$absolute_path"
  fi
}

sdv_is_nextos() {
  local root=${1:-/} os_release

  os_release=$(sdv_platform_path "$root" /etc/os-release)
  [ -r "$os_release" ] &&
    LC_ALL=C command grep -Eiq \
      'nextos|retro[[:space:]_-]*elite' "$os_release"
}

sdv_file_has_x5m_signature() {
  local path=$1

  [ -r "$path" ] || return 1
  LC_ALL=C command tr '\000' '\n' < "$path" 2>/dev/null |
    LC_ALL=C command grep -Eiq \
      '^(amlogic,[[:space:]]*s7d|amlogic,[[:space:]]*s905x5m|s905x5m)$'
}

sdv_is_tested_x5m_nextos() {
  local root=${1:-/} machine=${2:-} marker

  case "$machine" in
    aarch64|arm64) ;;
    *) return 1 ;;
  esac
  sdv_is_nextos "$root" || return 1

  for marker in \
    "$(sdv_platform_path "$root" /proc/device-tree/compatible)" \
    "$(sdv_platform_path "$root" /sys/firmware/devicetree/base/compatible)" \
    "$(sdv_platform_path "$root" /sys/devices/soc0/soc_id)"; do
    sdv_file_has_x5m_signature "$marker" && return 0
  done
  return 1
}

sdv_file_size() {
  local path=$1 value

  if [ ! -f "$path" ]; then
    printf '0\n'
    return 0
  fi
  value=$(command wc -c < "$path" 2>/dev/null) || value=0
  value=${value//[[:space:]]/}
  case "$value" in ''|*[!0-9]*) value=0 ;; esac
  printf '%s\n' "$value"
}

sdv_run_extractor() {
  local game_dir=$1 control_folder=$2 root=$3 machine=$4
  local firmware_ld_path status ui_log ui_size_before ui_size_after
  shift 4

  ui_log="$game_dir/.nxextract/stardewvalley-nextos/ui.log"
  ui_size_before=$(sdv_file_size "$ui_log")

  case "$machine" in
    aarch64|arm64)
      firmware_ld_path="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$control_folder/libs:$control_folder/libs.aarch64"
      if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        firmware_ld_path="$firmware_ld_path:$LD_LIBRARY_PATH"
      fi
      printf '[launcher] NXExtract using native AArch64 firmware libraries\n'
      (
        if sdv_is_tested_x5m_nextos "$root" "$machine"; then
          unset SDL_VIDEODRIVER SDL_VIDEO_DRIVER
          printf '[launcher] NXExtract X5M SDL autodetection enabled\n'
        fi
        export LD_LIBRARY_PATH="$firmware_ld_path"
        export NXEXTRACT_GAME_DIR="$game_dir"
        "$game_dir/run-extractor.sh" "$@"
      )
      status=$?
      ;;
    *)
      NXEXTRACT_GAME_DIR="$game_dir" \
        "$game_dir/run-extractor.sh" "$@"
      status=$?
      ;;
  esac

  ui_size_after=$(sdv_file_size "$ui_log")
  if [ "$ui_size_after" -gt "$ui_size_before" ]; then
    printf '%s\n' '=== NXExtract UI diagnostic (last 80 lines) ==='
    command tail -c "+$((ui_size_before + 1))" "$ui_log" 2>/dev/null |
      command tail -n 80 || true
    printf '%s\n' '=== end NXExtract UI diagnostic ==='
  fi
  return "$status"
}
