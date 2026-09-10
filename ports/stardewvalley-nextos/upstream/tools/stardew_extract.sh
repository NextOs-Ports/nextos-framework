#!/bin/bash
# Extrai os dados do jogo do APK do Stardew Valley Android.
#
#   pacote  com.chucklefish.stardewvalley
#   versao  1.6.15.3   (arm64-v8a)
#
# O port distribui SO' o nosso codigo; os dados saem do APK que voce ja possui.
# Ponha o .apk na pasta do port e rode o jogo uma vez — o launcher chama isto.
# Depende so' de `unzip`, presente em todo CFW (sem aapt/python/love2d).
#
#   uso: stardew_extract.sh <GAMEDIR>
set -u

GAMEDIR="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
PKG="com.chucklefish.stardewvalley"
VER="1.6.15.3"

say() {
  echo "$*"
  [ -n "${CUR_TTY:-}" ] && [ -w "${CUR_TTY:-}" ] && echo "$*" > "$CUR_TTY"
  return 0
}

# Ja' extraido? nada a fazer.
[ -f "$GAMEDIR/libs/libmonosgen-2.0.so" ] && [ -d "$GAMEDIR/assets/Content" ] && exit 0

APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null | head -1)
if [ -z "$APK" ]; then
  say "=========================================================="
  say " Stardew Valley: faltam os dados do jogo."
  say ""
  say " Copie o APK para:  $GAMEDIR"
  say "   $PKG  versao $VER  (arm64-v8a)"
  say ""
  say " Abra o jogo de novo que ele extrai sozinho (~420 MB)."
  say "=========================================================="
  exit 1
fi

command -v unzip >/dev/null 2>&1 || {
  say "Stardew Valley: este firmware nao tem 'unzip' — extraia o APK no PC."
  exit 1
}

# Confere que e' o APK certo ANTES de gastar minutos descompactando.
unzip -l "$APK" "lib/arm64-v8a/libmonosgen-2.0.so" >/dev/null 2>&1 || {
  say "Stardew Valley: '$(basename "$APK")' nao e' o APK arm64 do jogo"
  say "  (esperado: $PKG $VER, com lib/arm64-v8a/)"
  exit 1
}

say "Stardew Valley: extraindo $(basename "$APK") — nao desligue o aparelho."
mkdir -p "$GAMEDIR/libs" || exit 1
say "  [1/2] bibliotecas nativas..."
unzip -o -j "$APK" "lib/arm64-v8a/*" -d "$GAMEDIR/libs" >/dev/null 2>&1
say "  [2/2] conteudo do jogo (o passo demorado)..."
unzip -o "$APK" "assets/Content/*" -d "$GAMEDIR" >/dev/null 2>&1

if [ -f "$GAMEDIR/libs/libmonosgen-2.0.so" ] && [ -d "$GAMEDIR/assets/Content" ]; then
  say "Stardew Valley: dados prontos."
  # O APK ja' nao serve pra nada e sao ~500 MB de cartao.
  [ -z "${SDV_KEEP_APK:-}" ] && rm -f "$APK"
  exit 0
fi

say "Stardew Valley: a extracao falhou — confira o espaco livre no cartao."
exit 1
