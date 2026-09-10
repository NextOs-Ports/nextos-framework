#!/bin/bash
# sf_es_mapping — deriva SDL_GAMECONTROLLERCONFIG do es_input.cfg para o pad CONECTADO.
# Existe porque em alguns CFW o get_controls devolve $sdl_controllerconfig VAZIO
# (medido no device de teste, pad "USB Gamepad" 0810:0001): sem mapping o SDL ve so um joystick
# cru e NENHUM botao chega ao jogo. O es_input.cfg ja tem o que o proprio usuario
# configurou no EmulationStation — nunca pedir captura de botao (regra #19).
sf_es_mapping() {
  local awkf="${1:-$(dirname "${BASH_SOURCE[0]}")/es2sdl.awk}" cfg nm
  [ -r "$awkf" ] || return 1
  # nomes dos pads REALMENTE conectados (os que expoem js*)
  local names; names=$(awk '
    /^N: Name=/ { n=$0; sub(/^N: Name="/,"",n); sub(/"$/,"",n) }
    /^H: Handlers=/ { if ($0 ~ /js[0-9]/ && n != "") { gsub(/^[ \t]+|[ \t]+$/,"",n); print n } }
  ' /proc/bus/input/devices 2>/dev/null)
  [ -n "$names" ] || return 1
  while IFS= read -r nm; do
    [ -n "$nm" ] || continue
    for cfg in /storage/.config/emulationstation/es_input.cfg \
               /storage/.emulationstation/es_input.cfg \
               /emuelec/configs/emulationstation/es_input.cfg \
               "$HOME/.quirks/es_input.cfg" \
               "$HOME/.emulationstation/es_input.cfg" \
               /home/ark/.quirks/es_input.cfg \
               /home/ark/.emulationstation/es_input.cfg \
               /roms/.emulationstation/es_input.cfg \
               /roms2/.emulationstation/es_input.cfg; do
      [ -r "$cfg" ] || continue
      out=$(awk -v wantname="$nm" -f "$awkf" "$cfg" 2>/dev/null) && [ -n "$out" ] && {
        printf '%s\n' "$out"; return 0; }
    done
  done <<< "$names"
  return 1
}
[ "${BASH_SOURCE[0]}" = "$0" ] && sf_es_mapping "$@"
