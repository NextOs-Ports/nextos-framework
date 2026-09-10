#!/bin/bash
# Sonic 4 Episode II -- AARCH64 nativo (libfox arm64 v3.0.0 + OBB v3 data.obb).

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
> "$GAMEDIR/log_arm64.txt" && exec > >(tee "$GAMEDIR/log_arm64.txt") 2>&1

# First run: extract libfox.so + OBB from the APK / cache (bring-your-own data).
extract_data_first_run() {
  [ -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] && [ -f "$GAMEDIR/data/data.obb" ] && return 0
  return 0

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

trap 'pkill -x gptokeyb 2>/dev/null || true' EXIT INT TERM
extract_data_first_run

if [ ! -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] || [ ! -f "$GAMEDIR/data/data.obb" ]; then
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

# ArkOS keeps libmali/GLESv2/EGL (armhf) in /usr/local/lib/arm-linux-gnueabihf; other
# systems just ignore the missing paths. Display/audio are auto-detected (nothing forced).
export LD_LIBRARY_PATH="$GAMEDIR:$GAMEDIR/lib/arm64-v8a:/usr/lib:/lib:/usr/local/lib:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

$ESUDO chmod +x "$GAMEDIR/sonic4.arm64" 2>/dev/null || chmod +x "$GAMEDIR/sonic4.arm64"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

# Storage read-ahead to smooth OBB streaming (read-only, best-effort).
if command -v blockdev >/dev/null 2>&1; then
  _sdev=$(df "$GAMEDIR" 2>/dev/null | awk 'NR==2{print $1}' | sed 's/p[0-9]*$//; s/[0-9]*$//')
  for _d in "$_sdev" /dev/mmcblk0 /dev/mmcblk1; do
    [ -b "$_d" ] && $ESUDO blockdev --setra 4096 "$_d" 2>/dev/null || true
  done
fi

if [ "${SONIC_GPTOKEYB:-0}" = "1" ]; then
  export SONIC_INPUT=gptk
  if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
    $GPTOKEYB "sonic4.arm64" -c "$GAMEDIR/sonic4.gptk" &
  elif command -v gptokeyb >/dev/null 2>&1; then
    gptokeyb -1 "sonic4.arm64" -c "$GAMEDIR/sonic4.gptk" &
  fi
fi

# Audio logging OFF por padrão (o log pesado de PlaySound/StopSound — centenas de linhas/seg via
# tee no SD — derruba a performance das fases pesadas). Banners de driver + vídeo (GL) + DEMOGUARD
# + crash continuam SEMPRE visíveis. `touch sonic4ep2/audiolog` religa p/ diagnosticar som.
[ -f "$GAMEDIR/audiolog" ] && export SONIC_AUDIOLOG=1

# Audio: auto by default. Only set this if your device has no sound: alsa | pulseaudio | pipewire
AUDIO_DRIVER="${AUDIO_DRIVER:-}"
[ -n "$AUDIO_DRIVER" ] && export SONIC_AUDIODRIVER="$AUDIO_DRIVER"

# Volume buttons (batocera/Knulli/raw-ALSA): point HOME at the system asoundrc so the
# ALSA "default" (softvol) opens and the device volume keys work.
if [ -z "$SONIC_KEEP_HOME" ]; then
  for _adir in /userdata/system /storage/.config /root "$HOME" /etc; do
    if [ -f "$_adir/.asoundrc" ]; then export HOME="$_adir"; break; fi
  done
fi

# Electric Road (Episode Metal) render fix; harmless elsewhere. Disable: SONIC_NO_CLEARALL.
[ -z "$SONIC_NO_CLEARALL" ] && export SONIC_CLEARALL=1

# Testing aid: `touch sonic4ep2/unlock_all` unlocks every stage (not shipped in releases).
[ -f "$GAMEDIR/unlock_all" ] && export SONIC_UNLOCK_ALL=1

# Testing aid: `touch sonic4ep2/simcrash` simula o crash de use-after-free do attract-demo no boot.
#   - com a guarda (padrão): RECUPERA e vai pro título (dá pra jogar) -> veja "SOBREVIVI" no log_arm64.txt.
#   - +`touch sonic4ep2/no_demoguard`: crash CRU (fecha) -> prova que a guarda salva.
# Remova os arquivos p/ voltar ao normal. (não vão na release.)
[ -f "$GAMEDIR/simcrash" ] && export SONIC_SIMDEMOCRASH=1
[ -f "$GAMEDIR/no_demoguard" ] && export SONIC_NO_DEMOGUARD=1

export SONIC_LPK=data/data.obb

./sonic4.arm64

pkill -x gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
