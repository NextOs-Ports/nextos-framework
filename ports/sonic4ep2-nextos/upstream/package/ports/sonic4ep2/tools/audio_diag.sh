#!/bin/sh
# Pega-log de audio do Sonic 4 EP2 (NextOS / PortMaster).
# Roda DEPOIS de jogar e sair: resume o ultimo log.txt dizendo quais drivers de
# audio o device expoe, qual ABRIU (auto/fallback/override) ou se ficou MUDO, e
# os erros de SDL/EGL/pulse/alsa/pipewire. Salva audio_diag.txt p/ compartilhar.
#
# Uso no device:  sh tools/audio_diag.sh   (a partir de roms/ports/sonic4ep2)
GAMEDIR="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)"
[ -z "$GAMEDIR" ] && GAMEDIR="."
LOG="$GAMEDIR/log.txt"
OUT="$GAMEDIR/audio_diag.txt"

{
  echo "==== Sonic4EP2 AUDIO DIAG ===="
  echo "log: $LOG"
  if [ ! -f "$LOG" ]; then
    echo "!! log.txt nao existe -- rode o jogo uma vez primeiro."
  else
    echo
    echo "---- DRIVERS de audio + qual ABRIU ----"
    grep -nE "sonic_audio: drivers|sonic_audio: SDL audio aberto|sonic_audio: open automatico|sonic_audio: override|sonic_audio: driver=|sonic_audio: NENHUM|opensles_shim: audio drivers|opensles_shim: audio aberto|opensles_shim: open auto|opensles_shim: driver=|opensles_shim: NENHUM" "$LOG" 2>/dev/null
    echo
    echo "---- VIDEO: contexto GL escolhido + identidade da GPU ----"
    grep -nE "egl_shim: GL context OK|egl_shim: try ES|egl_shim: NENHUMA|egl_shim: GL_VENDOR|egl_shim: GL_RENDERER|egl_shim: GL_VERSION|egl_shim: GL_GLSL|egl_shim: depth.*SDL_CreateWindow|EGL_BAD|Could not create EGL|SEM VIDEO" "$LOG" 2>/dev/null
    echo
    echo "---- ERROS / pistas (SDL/EGL/pulse/alsa/pipewire/runtime) ----"
    grep -niE "SDL_OpenAudio|InitSubSystem|EGL_BAD|Could not|failed|cannot|can.t make|pipewire|pulse|alsa|XDG_RUNTIME|PULSE_SERVER|no such|permission" "$LOG" 2>/dev/null | head -40
    echo
    echo "---- BOOT / EXIT ----"
    grep -nE "loop principal|SetGamePath|setScreenSize|gamepad|SELECT\+START|exit" "$LOG" 2>/dev/null | head
  fi
  echo "==== fim ===="
} > "$OUT" 2>&1

echo "Resumo salvo em: $OUT"
echo "------------------------------------------"
cat "$OUT"
