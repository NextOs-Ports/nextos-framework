#!/usr/bin/env bash
# Monta e AUDITA o pacote publico universal BYO-data de um dos dois jogos.
#
#   package/build-package.sh geometrydash [saida.zip]
#   package/build-package.sh gdsubzero    [saida.zip]
#
# Molde estrutural: chrono-nextos/package/build-package.sh (release publicada).
set -euo pipefail

export LC_ALL=C
export TZ=UTC

fail() {
  printf 'package error: %s\n' "$*" >&2
  exit 1
}

# `ambos` monta UM zip com os dois jogos: dois launchers e duas pastas lado a
# lado. Cada jogo mantem o seu proprio `gamedata/` e a sua propria receita, que
# recusa o APK do outro pelo pacote Android -- o que se compartilha e' o
# download, nunca os dados.
SELECTION=${1:-}
case "$SELECTION" in
  geometrydash|gdsubzero) GAMES=("$SELECTION") ;;
  ambos|both|all)         GAMES=(geometrydash gdsubzero) ;;
  *) fail "uso: $0 {geometrydash|gdsubzero|ambos} [saida.zip]" ;;
esac

game_title() {
  case "$1" in
    geometrydash) printf 'Geometry Dash' ;;
    gdsubzero)    printf 'Geometry Dash SubZero' ;;
  esac
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
if [[ ${#GAMES[@]} -eq 1 ]]; then
  OUTPUT=${2:-"$REPO_DIR/.build/${GAMES[0]}.zip"}
else
  OUTPUT=${2:-"$REPO_DIR/.build/geometrydash-subzero.zip"}
fi
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

for game in "${GAMES[@]}"; do
  if [[ ${GD_SKIP_BUILD:-0} != 1 ]]; then
    "$REPO_DIR/build_universal.sh" "$game"
  fi
  [[ -f "$REPO_DIR/$game-universal" ]] ||
    fail "runtime universal ausente: $REPO_DIR/$game-universal"
done

for tool in awk bash dirname file find grep install mkdir mktemp readelf rm \
            sed sha256sum sort touch unzip zip; do
  command -v "$tool" >/dev/null 2>&1 || fail "ferramenta ausente no host: $tool"
done

case "$SOURCE_DATE_EPOCH" in
  ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH precisa ser um timestamp Unix" ;;
esac
(( SOURCE_DATE_EPOCH >= 315532800 )) || fail "SOURCE_DATE_EPOCH antes do ZIP"
(( SOURCE_DATE_EPOCH <= 4354819198 )) || fail "SOURCE_DATE_EPOCH depois do ZIP"
(( SOURCE_DATE_EPOCH % 2 == 0 )) || fail "SOURCE_DATE_EPOCH precisa ser par"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/gd-package.XXXXXX")
STAGE="$TMP_ROOT/stage"
TMP_ZIP="$TMP_ROOT/package.zip"
trap 'rm -rf -- "$TMP_ROOT"' EXIT INT TERM
mkdir -p "$STAGE"

put() {
  local mode=$1 source=$2 destination=$3
  [[ -f "$source" ]] || fail "fonte do pacote ausente: $source"
  install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

for GAME in "${GAMES[@]}"; do
  GAME_TITLE=$(game_title "$GAME")
  GAME_DIR="$REPO_DIR/games/$GAME"
  BINARY=${GD_PACKAGE_BINARY:-"$REPO_DIR/$GAME-universal"}

  put 0755 "$GAME_DIR/$GAME_TITLE.sh"            "$GAME_TITLE.sh"
  put 0755 "$BINARY"                             "$GAME/$GAME"
  put 0755 "$GAME_DIR/run.sh"                    "$GAME/run.sh"
  put 0644 "$REPO_DIR/README.md"                 "$GAME/README.md"
  put 0644 "$REPO_DIR/NOTICE.md"                 "$GAME/NOTICE.md"
  put 0644 "$REPO_DIR/INSTALLATION.md"           "$GAME/INSTALLATION.md"
  put 0644 "$REPO_DIR/LICENSE"                   "$GAME/LICENSE"
  put 0644 "$REPO_DIR/licenses/NXExtract-MIT.txt" "$GAME/licenses/NXExtract-MIT.txt"
  put 0644 "$GAME_DIR/gamedata/README.txt"       "$GAME/gamedata/README.txt"
  put 0644 "$REPO_DIR/version.txt"               "$GAME/version.txt"
  put 0755 "$REPO_DIR/nxextract.py"              "$GAME/nxextract.py"
  put 0755 "$REPO_DIR/nxextract-ui"              "$GAME/nxextract-ui"
  put 0755 "$REPO_DIR/nxextract-runtime-env.sh"  "$GAME/nxextract-runtime-env.sh"
  put 0755 "$REPO_DIR/run-extractor.sh"          "$GAME/run-extractor.sh"
  put 0644 "$GAME_DIR/extractor.json"            "$GAME/extractor.json"
  put 0644 "$REPO_DIR/nxextract-version.txt"     "$GAME/nxextract-version.txt"
  put 0755 "$REPO_DIR/tools/make-game-apk.py"    "$GAME/tools/make-game-apk.py"
done

# GUARD DOS SHA PINADOS (licao Stardew v1.1.7): `nxextract-version.txt` registra
# o sha256 de cada arquivo vendorizado. Mexer no extrator ou na receita e
# esquecer de regerar o pino ja mandou release com o pino MENTINDO sobre o
# conteudo. Aqui o pacote FALHA em vez de sair com registro errado.
verify_pins() {
  local pins=$REPO_DIR/nxextract-version.txt name expected actual bad=0
  [ -f "$pins" ] || fail "nxextract-version.txt ausente"
  while read -r name expected; do
    case "$name" in ''|'#'*) continue ;; esac
    [ -f "$REPO_DIR/$name" ] || fail "pino aponta para arquivo ausente: $name"
    actual=$(sha256sum -- "$REPO_DIR/$name" | cut -d' ' -f1)
    if [ "$actual" != "$expected" ]; then
      printf 'pino DESATUALIZADO: %s\n  registrado: %s\n  real:       %s\n' \
        "$name" "$expected" "$actual" >&2
      bad=1
    fi
  done <<EOF
$(sed -n 's/^\([^ ]*\) sha256=\([0-9a-f]\{64\}\)$/\1 \2/p' "$pins")
EOF
  [ "$bad" -eq 0 ] ||
    fail "regenere nxextract-version.txt antes de empacotar (sha pinado != arquivo real)"
  printf 'pinos do NXExtract conferidos contra os arquivos reais\n'
}
verify_pins

glibc_at_most() {
  local candidate=$1 maximum=$2 newest version major minor machine
  machine=$(readelf -h "$candidate" |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
  [[ $machine == AArch64 ]] ||
    fail "${candidate#"$STAGE/"} nao e' AArch64 (achado: $machine)"
  newest=$(readelf --version-info "$candidate" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
  [[ -n $newest ]] || fail "ABI glibc indeterminada: ${candidate#"$STAGE/"}"
  version=${newest#GLIBC_}
  major=${version%%.*}
  minor=${version#*.}; minor=${minor%%.*}
  if (( major > 2 || (major == 2 && minor > maximum) )); then
    fail "${candidate#"$STAGE/"} exige $newest (maximo: GLIBC_2.$maximum)"
  fi
}

# Auditoria: o loader e a UI do NXExtract sao os UNICOS ELFs permitidos. As
# bibliotecas Android originais entram depois, fornecidas pelo dono do jogo.
while IFS= read -r -d '' candidate; do
  kind=$(file -b "$candidate")
  case "$kind" in
    *ELF*)
      relative=${candidate#"$STAGE/"}
      allowed_elf=0
      for game in "${GAMES[@]}"; do
        case "$relative" in
          "$game/$game"|"$game/nxextract-ui") allowed_elf=1 ;;
        esac
      done
      [[ $allowed_elf -eq 1 ]] ||
        fail "ELF inesperado entrou no pacote: $relative"
      glibc_at_most "$candidate" 30
      ;;
    *PE32*|*Mach-O*)
      fail "executavel estrangeiro no pacote: ${candidate#"$STAGE/"}"
      ;;
  esac
done < <(find "$STAGE" -type f -print0)

# Bits +x esperados: nenhuma etapa critica depende deles, mas o zip precisa
# entregar o conjunto certo (a lista nao pode encolher junto com refactor).
for GAME in "${GAMES[@]}"; do
  GAME_TITLE=$(game_title "$GAME")
  for expected in "$GAME_TITLE.sh" "$GAME/run.sh" "$GAME/$GAME" \
                  "$GAME/nxextract.py" "$GAME/nxextract-ui" \
                  "$GAME/nxextract-runtime-env.sh" "$GAME/run-extractor.sh" \
                  "$GAME/tools/make-game-apk.py"; do
    [[ -x "$STAGE/$expected" ]] || fail "faltou bit +x esperado: $expected"
  done
done

for GAME in "${GAMES[@]}"; do
  GAME_TITLE=$(game_title "$GAME")

  # O runtime e' bash (BASH_SOURCE, `source` do control.txt): o script visivel
  # TEM que chama-lo com bash. Ja' quebrou uma instalacao inteira com
  # "Bad substitution" e log vazio.
  grep -q 'exec bash "\$launcher"' "$STAGE/$GAME_TITLE.sh" ||
    fail "o launcher visivel precisa executar o runtime com bash"

  bash -n "$STAGE/$GAME_TITLE.sh"
  bash -n "$STAGE/$GAME/run.sh"
  python3 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())' \
    "$STAGE/$GAME/tools/make-game-apk.py"
  python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
    "$STAGE/$GAME/extractor.json"

  if grep -En '^[[:space:]]*(export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER=' \
      "$STAGE/$GAME_TITLE.sh" "$STAGE/$GAME/run.sh"; then
    fail "o launcher nao pode fixar backend SDL de video ou audio"
  fi
  if grep -En \
      '(^|[[:space:]])(setsid|nohup|systemctl[[:space:]]+(stop|mask|restart)|pkill)([[:space:]]|$)' \
      "$STAGE/$GAME_TITLE.sh" "$STAGE/$GAME/run.sh" |
      grep -v '^[^:]*:[0-9]*:[[:space:]]*#'; then
    fail "o launcher contem comando de ciclo de vida proibido"
  fi
  if grep -En 'gptokeyb' "$STAGE/$GAME_TITLE.sh" "$STAGE/$GAME/run.sh" |
      grep -v '^[^:]*:[0-9]*:#'; then
    fail "gptokeyb roubaria o pad: o controle deste port e' NATIVO"
  fi
done

# Nenhum dado de jogo, nenhuma lib proprietaria, nenhum artefato de dev.
if find "$STAGE" \( \
    -iname '*.apk' -o -iname '*.apkm' -o -iname '*.apks' -o \
    -iname '*.xapk' -o -iname '*.obb' -o -iname '*.dex' -o \
    -name 'libcocos2dcpp.so' -o -name 'libfmod.so' -o \
    -iname '*.plist' -o -iname '*.mp3' -o -iname '*.ogg' \
  \) -print -quit | grep -q .; then
  fail "dado de jogo proprietario entrou na arvore publica"
fi
if find "$STAGE" \( \
    -iname '*.log' -o -iname '*.raw' -o -iname '*.ppm' -o -iname '*.pcm' -o \
    -name 'HANDOFF.md' -o -name '__pycache__' -o -name '*.pyc' -o \
    -name 'userdata' -o -name 'debug*.log' -o -name 'scratchpad' \
  \) -print -quit | grep -q .; then
  fail "artefato de desenvolvimento ou pessoal entrou na arvore publica"
fi
if grep -IRnE '192[.]168[.]|169[.]254[.]|10[.][0-9]+[.]|/home/|/media/|root@|[A-Za-z0-9._%+-]+@(gmail|hotmail|outlook|yahoo|proton)[.]' \
    "$STAGE" --include='*.sh' --include='*.md' --include='*.txt' \
    --include='*.json' --include='*.py'; then
  fail "texto da release contem endereco de teste ou caminho pessoal"
fi

# Auditoria oficial de portabilidade sobre a arvore que vai virar ZIP. As
# excecoes ficam escritas em package/audit-allow.txt, com o contrato de cada
# uma -- e' o unico jeito de o "filho em background" do launcher passar.
"$SCRIPT_DIR/audit-portability.sh" --allow "$SCRIPT_DIR/audit-allow.txt" "$STAGE" ||
  fail "audit-portability.sh reprovou a arvore do pacote"

(
  cd "$STAGE"
  find . -type f ! -name 'PACKAGE-MANIFEST.sha256' \
    -printf '%P\n' | sort | while IFS= read -r relative; do
      sha256sum -- "$relative"
    done
) > "$STAGE/PACKAGE-MANIFEST.sha256"

find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(
  cd "$STAGE"
  find . -type f -printf '%P\n' | sort | zip -X -9 -q "$TMP_ZIP" -@
)
unzip -tq "$TMP_ZIP" >/dev/null

VERIFY="$TMP_ROOT/verify"
mkdir -p "$VERIFY"
unzip -q "$TMP_ZIP" -d "$VERIFY"
(
  cd "$VERIFY"
  sha256sum -c PACKAGE-MANIFEST.sha256 >/dev/null
)

mkdir -p "$(dirname -- "$OUTPUT")"
OUTPUT_DIR=$(cd -- "$(dirname -- "$OUTPUT")" && pwd -P)
OUTPUT="$OUTPUT_DIR/$(basename -- "$OUTPUT")"
install -m 0644 "$TMP_ZIP" "$OUTPUT"
(
  cd "$OUTPUT_DIR"
  sha256sum "$(basename -- "$OUTPUT")" > "$(basename -- "$OUTPUT").sha256"
)

printf 'PACKAGE OK: %s\n' "$OUTPUT"
printf 'loader ABI: GLIBC <= 2.30\n'
sha256sum "$BINARY" "$OUTPUT"
