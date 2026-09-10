#!/bin/bash
# Sonic The Hedgehog 4: Episode II -- Android so-loader -> NextOS / PortMaster.
# BYO-data: copy the APK and cache ZIP/OBB to roms/ports/sonic4ep2, then launch once.

PORTNAME="Sonic The Hedgehog 4: Episode II"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
else
  controlfolder="/storage/.config/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/sonic4ep2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

kill_existing_sonic4() {
  self="$$"
  pkill -x gptokeyb 2>/dev/null || true
  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$GAMEDIR"/sonic4|"$GAMEDIR"/sonic4.*)
        echo "killing stale sonic4 pid $pid: $target"
        kill "$pid" 2>/dev/null || true
        ;;
    esac
  done
  sleep 1
  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$GAMEDIR"/sonic4|"$GAMEDIR"/sonic4.*)
        echo "force killing stale sonic4 pid $pid: $target"
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done
}

extract_data_first_run() {
  need_lib=0
  need_obb=0
  [ -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] || need_lib=1
  [ -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ] || need_obb=1
  [ "$need_lib" = 0 ] && [ "$need_obb" = 0 ] && return 0

  echo "First run setup: extracting Sonic 4 Episode II data."
  $ESUDO chmod +x "$GAMEDIR/tools/sonic4ep2_extract.src" "$GAMEDIR/tools/progressor" 2>/dev/null || true

  PROGRESSOR="$(command -v progressor 2>/dev/null || true)"
  [ -z "$PROGRESSOR" ] && [ -x "$GAMEDIR/tools/progressor" ] && PROGRESSOR="$GAMEDIR/tools/progressor"

  if [ -n "$PROGRESSOR" ]; then
    SONIC4EP2_PROGRESSOR=1 "$PROGRESSOR" \
      --title "Sonic 4 Episode II" \
      --log "$GAMEDIR/tools/extract.log" \
      "$GAMEDIR/tools/sonic4ep2_extract.src" || "$GAMEDIR/tools/sonic4ep2_extract.src"
  else
    "$GAMEDIR/tools/sonic4ep2_extract.src"
  fi
}

kill_existing_sonic4
trap 'kill_existing_sonic4; pkill -x gptokeyb 2>/dev/null || true' EXIT INT TERM
extract_data_first_run

if [ ! -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] || [ ! -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ]; then
  echo "############################################################"
  echo " Missing Sonic 4 Episode II data."
  echo " Copy these files to: $GAMEDIR/"
  echo "  - sonic-the-hedgehog-4-episode-ii-2.0.0.apk"
  echo "  - cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip"
  echo "    or main.22.com.sega.sonic4episode2.obb"
  echo " Then launch Sonic 4 EP2 again."
  echo "############################################################"
  echo "Missing Sonic 4 EP2 APK/cache. See $GAMEDIR/README.md" > "$CUR_TTY" 2>/dev/null || true
  sleep 10
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR:$GAMEDIR/lib/armeabi-v7a:/usr/lib32:/lib32:/usr/lib/arm-linux-gnueabihf:/lib/arm-linux-gnueabihf:/usr/lib:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1

# --- Display: portavel Mali-fbdev (Amlogic) <-> Wayland/kmsdrm (R36S/ArchR ES-Sway) ---
# NUNCA forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER (regra do device): o SDL auto-detecta
# wayland/pulse no R36S e fbdev no Amlogic. So garantimos que, em sessao Wayland, o
# XDG_RUNTIME_DIR/WAYLAND_DISPLAY existam (runemu/ES geralmente exporta; fallback abaixo).
# Em fbdev (Amlogic) nao ha socket wayland-N -> as vars ficam vazias -> SDL cai no fbdev.
export SDL_NO_SIGNAL_HANDLERS=1
if [ -z "$XDG_RUNTIME_DIR" ]; then
  for d in /run/0-runtime-dir /var/run/0-runtime-dir /run/user/0 /var/run/user/0 /run/user/$(id -u 2>/dev/null); do
    [ -d "$d" ] && { export XDG_RUNTIME_DIR="$d"; break; }
  done
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -n "$XDG_RUNTIME_DIR" ]; then
  WD=$(ls "$XDG_RUNTIME_DIR"/ 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
  [ -n "$WD" ] && export WAYLAND_DISPLAY="$WD"
fi

sonic_valid_res() {
  case "$1" in
    [0-9]*x[0-9]*)
      rw="${1%x*}"
      rh="${1#*x}"
      rh="${rh%%[^0-9]*}"
      [ -n "$rw" ] && [ -n "$rh" ] && [ "$rw" -ge 320 ] && [ "$rh" -ge 240 ]
      ;;
    *) return 1 ;;
  esac
}

sonic_real_res_from_wlroots() {
  command -v wlr-randr >/dev/null 2>&1 || return 1
  wlr-randr 2>/dev/null | awk '
    /current/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^[0-9]+x[0-9]+/) {
          sub(/[^0-9x].*/, "", $i);
          print $i;
          exit;
        }
      }
    }'
}

sonic_real_res_from_drm() {
  for st in /sys/class/drm/card*-*/status; do
    [ -r "$st" ] || continue
    grep -q connected "$st" 2>/dev/null || continue
    modefile="${st%/status}/modes"
    mode="$(sed -n '1p' "$modefile" 2>/dev/null)"
    sonic_valid_res "$mode" && { echo "$mode"; return 0; }
  done
  for modefile in /sys/class/drm/card*-*/modes; do
    [ -r "$modefile" ] || continue
    mode="$(sed -n '1p' "$modefile" 2>/dev/null)"
    sonic_valid_res "$mode" && { echo "$mode"; return 0; }
  done
  return 1
}

sonic_apply_real_res_override() {
  [ -n "$SONIC_RES" ] && return 0
  [ -n "$WAYLAND_DISPLAY" ] || [ -e /dev/dri/card0 ] || return 0

  real_res="$(sonic_real_res_from_wlroots)"
  sonic_valid_res "$real_res" || real_res="$(sonic_real_res_from_drm)"
  sonic_valid_res "$real_res" || return 0

  export SONIC_RES="$real_res"
  echo "Sonic display override: SONIC_RES=$SONIC_RES"
}

sonic_apply_real_res_override

export SONIC_DATADIR="$GAMEDIR"
export SONIC_AUTOSTART=0
export SONIC_NOFAKESOUND=1
export SONIC_SWAPINT=1

$ESUDO chmod +x "$GAMEDIR/sonic4" 2>/dev/null || chmod +x "$GAMEDIR/sonic4"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  $GPTOKEYB "sonic4" -c "$GAMEDIR/sonic4.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  gptokeyb -1 "sonic4" -c "$GAMEDIR/sonic4.gptk" &
fi

./sonic4

pkill -x gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
