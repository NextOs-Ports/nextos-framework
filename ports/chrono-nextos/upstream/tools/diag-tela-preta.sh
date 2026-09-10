#!/bin/bash
# diag-tela-preta.sh -- FERRAMENTA DE TESTE, nao entra no pacote publico.
#
# Responde UMA pergunta sobre o relato "depois do titulo a tela nao volta":
# o jogo esta desenhando e a apresentacao nao chega no painel, ou o jogo
# passou a desenhar preto de verdade?
#
# glReadPixels e' lido ANTES do swap, entao ele ve o que o jogo DESENHOU,
# independente do que o painel mostra. Se os shots depois do titulo tiverem
# pixel nao-preto e a tela estiver preta, o defeito e' de APRESENTACAO
# (backend de video / swap), nao de render.
#
# O binario publico traz tudo isto atras de variavel de ambiente:
# nao e' preciso build nova nem trocar arquivo do pacote.
#
# Uso, no aparelho, pelo shell:
#     bash tools/diag-tela-preta.sh 1     # rodada base (reproduz o defeito)
#     bash tools/diag-tela-preta.sh 2     # rodada com glFinish antes do swap
#
# Em CADA rodada: o jogo abre, VOCE aperta A no titulo como sempre, espera a
# tela ficar preta, conta uns 20 segundos e sai com SELECT+START.

set -u

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$GAMEDIR" || exit 1

MODE=${1:-}
case "$MODE" in
  1) LABEL=base;     EXTRA_ENV="" ;;
  2) LABEL=glfinish; EXTRA_ENV="CHRONO_GLFINISH=1" ;;
  *) echo "uso: bash tools/diag-tela-preta.sh [1|2]" >&2; exit 2 ;;
esac

OUT=$GAMEDIR/diag-$LABEL
rm -rf "$OUT"; mkdir -p "$OUT"

# O jogo tem trava de instancia unica no proprio binario: uma copia esquecida
# viva faz a rodada seguinte morrer sem explicar. Derrubar e CONFERIR.
echo "== derrubando qualquer copia viva do jogo =="
pkill -f "$GAMEDIR/chrono-nextos" 2>/dev/null
sleep 2
pkill -9 -f "$GAMEDIR/chrono-nextos" 2>/dev/null
sleep 2
if pgrep -f "$GAMEDIR/chrono-nextos" >/dev/null 2>&1; then
  echo "ERRO: ainda ha copia do jogo viva; nao da para medir. Reinicie o aparelho." >&2
  exit 1
fi
echo "ok, nenhuma copia viva"

# Estado do aparelho que explica a diferenca entre um CFW e outro.
{
  echo "== rodada: $LABEL =="
  echo "== entradas evdev =="
  for e in /dev/input/event*; do
    [ -e "$e" ] || continue
    n=$(basename "$e")
    printf '%s %s\n' "$n" "$(cat "/sys/class/input/$n/device/name" 2>/dev/null)"
  done
  echo "== framebuffer / drm =="
  ls -la /dev/fb* /dev/dri/* 2>/dev/null
  cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
} > "$OUT/ambiente.txt" 2>&1

rm -f "$GAMEDIR"/shot_*.raw
: > "$GAMEDIR/debug.log"

echo
echo "== abrindo o jogo =="
echo "   1) aperte A no titulo, como sempre"
echo "   2) quando a tela ficar preta, espere uns 20 segundos"
echo "   3) saia com SELECT+START"
echo

# Um shot a cada 120 quadros (~2 s) cobre titulo, transicao e o preto.
env $EXTRA_ENV \
    CHRONO_SHOTEVERY=120 CHRONO_SHOTMAX=40 \
    CHRONO_FPSLOG=1 \
    bash "$GAMEDIR/../Chrono Trigger.sh"

echo
echo "== o que o jogo DESENHOU em cada captura =="
# cmp contra /dev/zero acha o primeiro byte nao-zero sem precisar de python:
# "EOF on /dev/zero" = a captura inteira e' preta.
for f in "$GAMEDIR"/shot_*.raw; do
  [ -e "$f" ] || { echo "nenhuma captura gerada -- o jogo nem entrou no laco"; break; }
  if cmp "$f" /dev/zero >/dev/null 2>&1; then
    verdict="PRETA (todos os pixels zerados)"
  else
    verdict="TEM IMAGEM (primeiro pixel nao-preto: $(cmp "$f" /dev/zero 2>&1 | sed 's/.*byte //'))"
  fi
  printf '%-24s %s\n' "$(basename "$f")" "$verdict"
done | tee "$OUT/capturas.txt"

cp -f "$GAMEDIR/debug.log" "$OUT/debug.log" 2>/dev/null
mv -f "$GAMEDIR"/shot_*.raw "$OUT/" 2>/dev/null

echo
echo "== perfil de video desta rodada =="
grep -E '^VIDEO |^Gamepad|glFinish antes do swap' "$OUT/debug.log" 2>/dev/null

echo
echo "pronto: $OUT"
echo "mande a pasta inteira (debug.log + capturas.txt + ambiente.txt)."
