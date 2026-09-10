#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gate hermetico da busca por fonte do sistema (nxcompat_system_font.h).
#
# O que se prova aqui e' a falha silenciosa: quando nenhum caminho casa, o port
# abre INTEIRO e MUDO -- imagem, audio, input e save funcionando, texto nenhum,
# nenhum erro no log. Por isso os cenarios negativos valem tanto quanto os
# positivos, e por isso um arquivo com nome de fonte mas sem assinatura de fonte
# TEM de ser recusado: aceita-lo devolveria exatamente o jogo mudo.
#
# Nenhum cenario toca /usr/share/fonts: tudo acontece num diretorio temporario.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
INCLUDE=$(cd -- "$HERE/../include" && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -D_POSIX_C_SOURCE=200809L \
  -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual \
  -I "$INCLUDE" -o "$WORK/probe" "$HERE/test_system_font.c"

# Uma fonte TrueType minima: so' a assinatura importa para este gate.
real_font() { printf '\x00\x01\x00\x00rest-of-face' > "$1"; }

fail() { echo "nxcompat_system_font=FAIL $*" >&2; exit 1; }
expect() { # expect <esperado> <saida> <cenario>
  [ "$2" = "$1" ] || fail "$3: esperado [$1], veio [$2]"
}

mkdir -p "$WORK/pm/pylibs/resources" "$WORK/roots/sub" "$WORK/vazio"
real_font "$WORK/explicita.ttf"
real_font "$WORK/pm/pylibs/resources/DejaVuSans.ttf"
real_font "$WORK/roots/sub/Alguma.ttf"
real_font "$WORK/pelo-env.ttf"
: > "$WORK/vazia.ttf"                          # nome de fonte, zero byte
printf '<html>nao sou fonte</html>' > "$WORK/mentirosa.ttf"

# 1. a escolha explicita de quem chama vence tudo
out=$(env -u NXCOMPAT_PORTMASTER_DIR -u CONTROLFOLDER \
      "$WORK/probe" find "$WORK/explicita.ttf" "" "" | head -1)
expect "found=1 path=$WORK/explicita.ttf" "$out" "explicita"

# 2. variavel de ambiente, quando nao ha escolha explicita
out=$(env -u NXCOMPAT_PORTMASTER_DIR -u CONTROLFOLDER \
      NX_TEST_FONT="$WORK/pelo-env.ttf" \
      "$WORK/probe" find "" NX_TEST_FONT "" | head -1)
expect "found=1 path=$WORK/pelo-env.ttf" "$out" "env"

# 3. a fonte que o proprio PortMaster carrega consigo -- o degrau que resolve
#    na pratica em aparelho de verdade
out=$(env -u CONTROLFOLDER NXCOMPAT_PORTMASTER_DIR="$WORK/pm" \
      "$WORK/probe" find "" "" "" | head -1)
expect "found=1 path=$WORK/pm/pylibs/resources/DejaVuSans.ttf" "$out" "portmaster"

# 4. raiz explicita de quem chama vence os caminhos do sistema, e a varredura
#    desce um nivel de subpasta
out=$(env -u NXCOMPAT_PORTMASTER_DIR -u CONTROLFOLDER \
      "$WORK/probe" find "" "" "$WORK/roots" | head -1)
expect "found=1 path=$WORK/roots/sub/Alguma.ttf" "$out" "varredura"

# 5. arquivo de zero byte com nome de fonte: RECUSADO. Aceitar aqui devolveria
#    o jogo mudo, que e' a falha que este modulo existe para impedir.
out=$("$WORK/probe" signature "$WORK/vazia.ttf")
expect "signature=0" "$out" "ttf de zero byte"

# 6. nome de fonte com conteudo que nao e' fonte: RECUSADO
out=$("$WORK/probe" signature "$WORK/mentirosa.ttf")
expect "signature=0" "$out" "ttf mentirosa"

# 7. e uma fonte de verdade: ACEITA
out=$("$WORK/probe" signature "$WORK/explicita.ttf")
expect "signature=1" "$out" "fonte real"

# 8. invariante no host real: o que a busca devolve passa na assinatura.
#    Separa "achou uma fonte" de "achou um arquivo com nome de fonte".
out=$(env -u NXCOMPAT_PORTMASTER_DIR -u CONTROLFOLDER \
      "$WORK/probe" find "" "" "")
case $out in
  "found=0 path=-") ;;                       # host sem fonte nenhuma: valido
  *"valid=1"*) ;;                            # achou, e o achado e' fonte
  *) fail "invariante: $out" ;;
esac

echo "nxcompat_system_font=PASS 8 cenarios"
