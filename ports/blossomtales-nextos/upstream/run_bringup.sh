#!/bin/bash
# Blossom Tales (Android, MonoGame/.NET) -> NextOS Mali-450.
# Launcher PortMaster padrao: o frontend chama este script em foreground e
# recebe o controle de volta quando o loader termina. Sem systemctl, sem matar
# ou restaurar o EmulationStation e sem watchdog — a saida e feita dentro do
# binario por SELECT+START.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt" 2>/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && \
  source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls 2>/dev/null

GAMEDIR="/storage/roms/ports/blossomtales"
cd "$GAMEDIR" || exit 1

# HOME proprio: os saves do jogo (IsolatedStorage) e as preferencias ficam
# dentro da pasta do port, nao espalhados pelo sistema.
mkdir -p "$GAMEDIR/home"
export HOME="$GAMEDIR/home"
# O IsolatedStorage do .NET (settings.ini, gamepad.map, 0.sav) fica em
# libs/.config/.isolated-storage/: o monodroid deriva os diretorios XDG do
# diretorio de dados do app e ignora XDG_CONFIG_HOME do ambiente. E a mesma
# posicao dos saves no stardewvalley e no tmntsr. Preserve essa pasta ao
# atualizar os dados do jogo.
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$GAMEDIR/libs:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
[ -f "/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt" ] && \
  export SDL_GAMECONTROLLERCONFIG_FILE="/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt"

export MALLOC_ARENA_MAX=2
export SB_LIBDIR="$GAMEDIR/libs"
export SB_ASSET_DIR="$GAMEDIR/assets"
export SB_APK="$GAMEDIR/game.apk"
export SB_RUNTIME_INIT=1
export SB_START_ACTIVITY=1
export SB_HOLD=1
export SB_JNI_VERBOSE=0
export SB_PRESENT_FBO=1
export SB_FBO_TRACK=1
export SB_RIGHT_CURSOR=0
export SB_WIDTH=1280
export SB_HEIGHT=720

# O FMOD abre a saida pela AAudio do port, que toca pelo servidor do sistema.
for socket in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$socket" ]; then
    export PULSE_SERVER="unix:$socket"
    break
  fi
done
mkdir -p /tmp/sb-run && export XDG_RUNTIME_DIR=/tmp/sb-run

# Tela de entrega do framework (nxsplash 0.1.2): cinco segundos fixos, sem
# switch de pulo e sem opcao por port. Um helper ausente e pacote malformado.
if [ -x "$GAMEDIR/nxsplash-nextos" ]; then
  "$GAMEDIR/nxsplash-nextos" "Blossom Tales: The Sleeping King" >/dev/null 2>&1 || true
fi

SB_LOG="$GAMEDIR/debug.log"
[ -f "$SB_LOG" ] && mv -f "$SB_LOG" "$GAMEDIR/debug.prev.log"
exec >>"$SB_LOG" 2>&1

printf '[launcher] inicio=%s launcher_pid=%s\n' \
  "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$$"
./blossomtales
SB_RC=$?
printf '[launcher] fim=%s exit_rc=%s\n' \
  "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$SB_RC"
exit "$SB_RC"
