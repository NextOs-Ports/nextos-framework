#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gate da sequencia de toque sintetico.
#
# O defeito que ele impede: o Brotato 1.0.4 preservava DOWN e UP, removia o
# ACTION_MOVE e prendia a subida a coordenada inicial. Clique simples ainda
# parecia funcionar em alguns firmwares; segurar e arrastar nunca poderia rolar
# uma lista.
#
# E o teste que existia exercitava apenas o helper de coordenadas -- por isso
# CODIFICOU a regressao em vez de detecta-la. Por isso este gate julga a
# SEQUENCIA INTEGRADA entregue quadro a quadro, nunca o calculo de coordenada
# isolado.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
INCLUDE=$(cd -- "$HERE/../include" && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$CC" -std=c99 -Wall -Wextra -Wconversion -Wshadow -Wcast-qual \
  -I "$INCLUDE" -o "$WORK/drive" "$HERE/test_touch_sequence.c"

fail() { echo "nxinput_touch_sequence=FAIL $*" >&2; exit 1; }
seq_of() { "$WORK/drive" "$@" | tr '\n' '|'; }
expect() { [ "$2" = "$1" ] || fail "$3
  esperado: $1
  veio:     $2"; }

# 1. CLIQUE CURTO -- a contagem no formato da MainActivity:
#    DOWN (1,1), UP (0,1), quadro seguinte (0,0).
out=$(seq_of 1,0.5,0.5 0,0.5,0.5 0,0.5,0.5)
expect "DOWN 1 1 0.500 0.500 t1|UP 0 1 0.500 0.500 t1|- 0 0 0.500 0.500 t1|" \
  "$out" "clique curto"

# 2. ARRASTO -- e' aqui que o Brotato quebrava: sem MOVE, e o UP preso na
#    coordenada inicial. O UP tem de sair na ULTIMA coordenada.
out=$(seq_of 1,0.1,0.1 1,0.5,0.5 1,0.9,0.9 0,0.9,0.9)
expect "DOWN 1 1 0.100 0.100 t1|MOVE 1 1 0.500 0.500 t1|MOVE 1 1 0.900 0.900 t1|UP 0 1 0.900 0.900 t1|" \
  "$out" "arrasto com MOVE e UP na ultima coordenada"

# 3. TREMOR abaixo do limiar NAO vira arrasto: ruido de analogico nao pode
#    rolar uma lista sozinho.
out=$(seq_of th=0.05 1,0.5,0.5 1,0.52,0.51 1,0.53,0.52 0,0.53,0.52)
expect "DOWN 1 1 0.500 0.500 t1|- 1 1 0.500 0.500 t1|- 1 1 0.500 0.500 t1|UP 0 1 0.500 0.500 t1|" \
  "$out" "jitter abaixo do limiar"

# 4. SOLTURA RAPIDA: pressionar e soltar no mesmo par de quadros continua
#    produzindo o par DOWN/UP completo -- nunca um DOWN orfao.
out=$(seq_of 1,0.2,0.2 0,0.2,0.2 1,0.8,0.8 0,0.8,0.8)
expect "DOWN 1 1 0.200 0.200 t1|UP 0 1 0.200 0.200 t1|DOWN 1 1 0.800 0.800 t3|UP 0 1 0.800 0.800 t3|" \
  "$out" "duas solturas rapidas"

# 5. DOWN_TIME constante dentro do gesto e DIFERENTE entre gestos: e' por ele
#    que a engine separa um arrasto de dois toques. No cenario 4 acima os dois
#    gestos sao t1 e t3; aqui um gesto longo mantem t1 do inicio ao fim.
out=$("$WORK/drive" 1,0.1,0.1 1,0.4,0.4 1,0.7,0.7 0,0.7,0.7 | awk '{print $6}' | sort -u | tr '\n' ' ')
expect "t1 " "$out" "down_time constante no gesto"

# 6. ROLAGEM VERTICAL: so' o eixo Y muda, e cada passo acima do limiar produz
#    um MOVE. Sem esses MOVE a lista nao rola, que era o sintoma de campo.
out=$("$WORK/drive" 1,0.5,0.9 1,0.5,0.7 1,0.5,0.5 1,0.5,0.3 0,0.5,0.3 | grep -c MOVE)
expect "3" "$out" "rolagem vertical produz MOVE por passo"

# 7. UM PONTEIRO SO': nunca sai DOWN atras de DOWN sem UP no meio. Segurar por
#    muitos quadros nao pode reabrir o gesto.
out=$("$WORK/drive" 1,0.5,0.5 1,0.5,0.5 1,0.5,0.5 1,0.9,0.9 0,0.9,0.9 | grep -c DOWN)
expect "1" "$out" "um unico DOWN por gesto"

# 8. DESCONEXAO / PAUSE no meio do gesto -- o dedo some sem UP explicito. O
#    quadro sem pressao FECHA o gesto: nenhum toque pode ficar preso, senao a
#    engine trava achando que o dedo continua encostado.
out=$(seq_of 1,0.5,0.5 1,0.6,0.6 0,0.6,0.6 0,0.6,0.6)
expect "DOWN 1 1 0.500 0.500 t1|MOVE 1 1 0.600 0.600 t1|UP 0 1 0.600 0.600 t1|- 0 0 0.600 0.600 t1|" \
  "$out" "gesto fechado ao perder a pressao"

# 9. Nenhum quadro deixa contagem presa: depois do UP, todo quadro solto e'
#    (0,0), que e' o que limpa os acumuladores da engine.
out=$("$WORK/drive" 1,0.5,0.5 0,0.5,0.5 0,0.5,0.5 0,0.5,0.5 | tail -2 | awk '{print $2 $3}' | sort -u | tr '\n' ' ')
expect "00 " "$out" "sem contagem presa apos o UP"

echo "nxinput_touch_sequence=PASS 9 cenarios"
