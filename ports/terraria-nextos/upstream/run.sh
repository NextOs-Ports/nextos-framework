#!/bin/sh
# Terraria Unity/IL2CPP loader runtime. SDL chooses the firmware backend.
set -u

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
export TER_GAMEDIR="$GAMEDIR"
cd "$GAMEDIR" || exit 1

ter_matches() {
  ter_pid=${1##*/}
  [ "$ter_pid" != "$$" ] || return 1
  ter_exe=$(readlink "$1/exe" 2>/dev/null || true)
  ter_comm=$(cat "$1/comm" 2>/dev/null || true)
  ter_cwd=$(readlink "$1/cwd" 2>/dev/null || true)
  ter_cmd=$(tr '\000' ' ' < "$1/cmdline" 2>/dev/null || true)
  case "$ter_exe" in
    "$GAMEDIR/terraria"|"$GAMEDIR/terraria (deleted)") return 0 ;;
  esac
  case "$ter_cmd" in
    *"$GAMEDIR/terraria"*) return 0 ;;
  esac
  if [ "$ter_cwd" = "$GAMEDIR" ]; then
    case "$ter_comm" in terraria|terraria-*) return 0 ;; esac
  fi
  return 1
}

ter_pids() {
  for ter_proc in /proc/[0-9]*; do
    [ -d "$ter_proc" ] || continue
    if ter_matches "$ter_proc"; then
      printf '%s\n' "${ter_proc##*/}"
    fi
  done
}

ter_describe() {
  for ter_pid in $(ter_pids); do
    ter_proc=/proc/$ter_pid
    ter_exe=$(readlink "$ter_proc/exe" 2>/dev/null || true)
    ter_comm=$(cat "$ter_proc/comm" 2>/dev/null || true)
    ter_cmd=$(tr '\000' ' ' < "$ter_proc/cmdline" 2>/dev/null || true)
    printf '[runtime] old pid=%s comm=%s exe=%s cmd=%s\n' \
      "$ter_pid" "$ter_comm" "$ter_exe" "$ter_cmd"
  done
}

ter_old=$(ter_pids)
if [ -n "$ter_old" ]; then
  ter_describe
  kill -TERM $ter_old 2>/dev/null || true
  ter_wait=0
  while [ -n "$(ter_pids)" ] && [ "$ter_wait" -lt 5 ]; do
    sleep 1
    ter_wait=$((ter_wait + 1))
  done
fi
ter_old=$(ter_pids)
if [ -n "$ter_old" ]; then
  printf '[runtime] graceful stop timed out; terminating stale pid(s): %s\n' "$ter_old"
  kill -KILL $ter_old 2>/dev/null || true
  sleep 1
fi
ter_old=$(ter_pids)
[ -z "$ter_old" ] || {
  printf '[runtime] refusing to launch while another Terraria loader is alive: %s\n' "$ter_old" >&2
  exit 74
}

for ter_required in terraria libunity.so libil2cpp.so libc++_shared.so \
  bin/Data/boot.config bin/Data/data.unity3d \
  bin/Data/Managed/Metadata/global-metadata.dat; do
  [ -s "$GAMEDIR/$ter_required" ] || {
    printf '[runtime] missing required owner data: %s\n' "$ter_required" >&2
    exit 73
  }
done

mkdir -p "$GAMEDIR/userdata" "$GAMEDIR/Players" "$GAMEDIR/Worlds"

# Keep firmware libraries ahead of Android guest libraries. No SDL video or
# audio backend is forced; the loader selects ownership from SDL's real driver.
ter_system_libs=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib
export LD_LIBRARY_PATH="$ter_system_libs${TERRARIA_FIRMWARE_LIBRARY_PATH:+:$TERRARIA_FIRMWARE_LIBRARY_PATH}:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Keep Unity's native job, GC and lifecycle sequence intact. Public launchers
# never enable the old diagnostic gates that skipped jobs or patched managed
# methods by version-specific offsets.
export CUP_NOLOGFILE=1

# Native InControl/Xbox path, on-screen name entry, and FMOD-to-SDL audio.
export TER_NATPAD=1
export TER_OSK=1
: "${TER_VK_DEFAULT:=Player}"
export TER_VK_DEFAULT
export TER_AUDIO=1
export TER_STREAMFALLBACK=1

printf '[runtime] backend=%s audio=%s game=%s\n' \
  "${SDL_VIDEODRIVER:-auto}" "${SDL_AUDIODRIVER:-auto}" "$GAMEDIR"
printf '[runtime] Quit Game or SELECT+START exits through guarded teardown\n'
exec "$GAMEDIR/terraria"
