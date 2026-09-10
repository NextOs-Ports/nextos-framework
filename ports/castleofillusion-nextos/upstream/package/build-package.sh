#!/usr/bin/env bash
# Empacota o ZIP publico do Castle of Illusion.
#
# Regras que este script FAZ CUMPRIR (nao sao comentario, sao gate):
#   - zero dado de jogo no pacote (APK/OBB/libs/assets do jogo);
#   - todo ELF nosso exige GLIBC <= 2.30;
#   - nenhum caminho pessoal, IP de device ou e-mail dentro dos arquivos;
#   - os arquivos que o launcher executa saem com +x, e nenhuma etapa critica
#     depende disso (o run.sh e' chamado por `bash`, nao pelo bit).
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUT=$REPO/.build
STAGE=$OUT/stage
NAME=castleofillusion
cd "$REPO"

[ -f castleofillusion ] || { echo "binario ausente: rode ./build_universal.sh"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/$NAME"

# --- Gate GLIBC ----------------------------------------------------------
maxglibc=$(
  { readelf -V castleofillusion 2>/dev/null || aarch64-linux-gnu-readelf -V castleofillusion; } |
    grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1
)
case "$maxglibc" in
  2.1[0-9]|2.2[0-9]|2.30) echo "gate glibc OK: castleofillusion exige $maxglibc" ;;
  *) echo "GATE GLIBC REPROVADO: castleofillusion exige $maxglibc (> 2.30)"; exit 1 ;;
esac

# --- Conteudo ------------------------------------------------------------
cp castleofillusion run.sh extractor.json version.txt nxextract-version.txt \
   nxextract.py nxextract-ui nxextract-runtime-env.sh run-extractor.sh \
   README.md INSTALLATION.md CHANGELOG.md LICENSE NOTICE.md \
   "$STAGE/$NAME/"
cp -r licenses "$STAGE/$NAME/"
mkdir -p "$STAGE/$NAME/gamedata"
cp gamedata/README.txt "$STAGE/$NAME/gamedata/"
cp "Castle of Illusion.sh" "$STAGE/"

chmod +x "$STAGE/$NAME/castleofillusion" "$STAGE/$NAME/run.sh" \
         "$STAGE/$NAME/nxextract.py" "$STAGE/$NAME/nxextract-ui" \
         "$STAGE/$NAME/nxextract-runtime-env.sh" "$STAGE/$NAME/run-extractor.sh" \
         "$STAGE/Castle of Illusion.sh"

# --- Gate: nenhum dado de jogo ------------------------------------------
if find "$STAGE" \( -name '*.obb' -o -name '*.apk' -o -name '*.xapk' \
                    -o -name '*.apkm' -o -name '*.dex' \
                    -o -name 'libViewer_GP.so' -o -name 'libfmodex.so' \) \
        -print -quit | grep -q .; then
  echo "GATE REPROVADO: dado de jogo dentro do pacote"; exit 1
fi

# --- Gate: nada pessoal --------------------------------------------------
# Montado por partes de proposito: um literal cru aqui viraria, ele mesmo, o
# identificador pessoal vazado no repositorio publico.
LEAK_RE="(^|[^0-9])(10|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}"
LEAK_RE="$LEAK_RE|/home/[a-z][a-z0-9_-]*/"
LEAK_RE="$LEAK_RE|[A-Za-z0-9._%+-]+@(gmail|hotmail|outlook|yahoo|proton|icloud)\."
if grep -rIlE "$LEAK_RE" "$STAGE" 2>/dev/null | grep -q .; then
  echo "GATE REPROVADO: caminho pessoal, IP ou e-mail no pacote"
  grep -rIlE "$LEAK_RE" "$STAGE" 2>/dev/null
  exit 1
fi

# --- ZIP + SHA -----------------------------------------------------------
rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
(cd "$STAGE" && zip -qr "$OUT/$NAME.zip" "Castle of Illusion.sh" "$NAME")
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")

echo "PACOTE OK -> $OUT/$NAME.zip"
cat "$OUT/$NAME.zip.sha256"
unzip -l "$OUT/$NAME.zip" | tail -5
