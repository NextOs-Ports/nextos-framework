#!/usr/bin/env bash
# Deterministic public BYO-data package for KOTOR.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
umask 022

fail() {
  printf 'package error: %s\n' "$*" >&2
  exit 1
}

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
VERSION=$(tr -d ' \n\r' < "$PORT_DIR/version.txt")
BINARY=${KOTOR_PACKAGE_BINARY:-"$PORT_DIR/kotor-nextos"}
OUTPUT=${1:-"$PORT_DIR/dist/kotor-nextos-$VERSION.zip"}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

if [[ ${KOTOR_SKIP_BUILD:-0} != 1 ]]; then
  "$PORT_DIR/build_universal.sh"
fi
[[ -f $BINARY ]] || fail "runtime universal ausente: $BINARY"

case $SOURCE_DATE_EPOCH in
  ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH precisa ser Unix" ;;
esac
(( SOURCE_DATE_EPOCH >= 315532800 && SOURCE_DATE_EPOCH <= 4354819198 &&
   SOURCE_DATE_EPOCH % 2 == 0 )) || fail "SOURCE_DATE_EPOCH fora do ZIP"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/kotor-package.XXXXXX")
STAGE=$TMP_ROOT/stage
TMP_ZIP=$TMP_ROOT/kotor.zip
trap 'rm -rf -- "$TMP_ROOT"' EXIT INT TERM
mkdir -p "$STAGE/kotor"

# The visible launcher is rendered by internal NextOS tooling that is not
# distributed. Where NX_GENERATOR points at a checkout, packaging still fails
# if the checked-in launcher or manifest drifted from the canonical render.
GENERATOR=${NX_GENERATOR:-$PORT_DIR/framework/nxbootstrap/tools/generate-port.py}
if [[ -f $GENERATOR ]]; then
  GENERATOR_OUTPUT=$TMP_ROOT/generated
  python3 "$GENERATOR" \
    "$PORT_DIR/nxport.json" --output "$GENERATOR_OUTPUT" >/dev/null
  cmp -s "$PORT_DIR/Star Wars KOTOR.sh" \
    "$GENERATOR_OUTPUT/Star Wars KOTOR.sh" ||
    fail "launcher differs from canonical nxbootstrap render"
  cmp -s "$PORT_DIR/nxport.json" "$GENERATOR_OUTPUT/kotor/nxport.json" ||
    fail "nxport.json is not canonical"
else
  echo "NX_GENERATOR not set; packaging the checked-in launcher as-is"
fi
"$PORT_DIR/tests/test-language.sh" >/dev/null

put() {
  local mode=$1 source=$2 destination=$3
  [[ -f $source ]] || fail "fonte ausente: $source"
  install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

put 0755 "$PORT_DIR/Star Wars KOTOR.sh" "Star Wars KOTOR.sh"
put 0755 "$BINARY" "kotor/kotor-nextos"
put 0644 "$PORT_DIR/port-env.sh" "kotor/port-env.sh"
put 0644 "$PORT_DIR/nxport.json" "kotor/nxport.json"
put 0644 "$PORT_DIR/port.json" "kotor/port.json"
put 0644 "$PORT_DIR/extractor.json" "kotor/extractor.json"
put 0644 "$PORT_DIR/nxextract-version.txt" "kotor/nxextract-version.txt"
put 0755 "$PORT_DIR/nxextract/nxextract.py" "kotor/nxextract/nxextract.py"
put 0755 "$PORT_DIR/nxextract/nxextract-ui" "kotor/nxextract/nxextract-ui"
put 0755 "$PORT_DIR/nxextract/nxextract-runtime-env.sh" \
  "kotor/nxextract/nxextract-runtime-env.sh"
put 0755 "$PORT_DIR/nxextract/run-extractor.sh" \
  "kotor/nxextract/run-extractor.sh"
put 0644 "$PORT_DIR/gamedata/README.txt" "kotor/gamedata/README.txt"
put 0644 "$PORT_DIR/README.md" "kotor/README.md"
put 0644 "$PORT_DIR/INSTALLATION.md" "kotor/INSTALLATION.md"
put 0644 "$PORT_DIR/CHANGELOG.md" "kotor/CHANGELOG.md"
put 0644 "$PORT_DIR/LICENSE" "kotor/LICENSE"
put 0644 "$PORT_DIR/NOTICE.md" "kotor/NOTICE.txt"
put 0644 "$PORT_DIR/licenses/NXExtract-MIT.txt" \
  "kotor/licenses/NXExtract-MIT.txt"
put 0644 "$PORT_DIR/version.txt" "kotor/version.txt"

verify_pins() {
  local name expected actual bad=0
  while read -r name expected; do
    [[ -n $name ]] || continue
    [[ -f $PORT_DIR/$name ]] || fail "pino aponta para ausente: $name"
    actual=$(sha256sum -- "$PORT_DIR/$name" | cut -d' ' -f1)
    if [[ $actual != "$expected" ]]; then
      printf 'pino NXExtract desatualizado: %s\n' "$name" >&2
      bad=1
    fi
  done < <(sed -n 's/^\([^ ]*\) sha256=\([0-9a-f]\{64\}\)$/\1 \2/p' \
    "$PORT_DIR/nxextract-version.txt")
  (( bad == 0 )) || fail "pinos do NXExtract divergentes"
}
verify_pins

glibc_at_most() {
  local candidate=$1 maximum=$2 newest version major minor
  newest=$(readelf --version-info "$candidate" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
  [[ -n $newest ]] || fail "ABI glibc indeterminada: ${candidate#"$STAGE/"}"
  version=${newest#GLIBC_}; major=${version%%.*}
  minor=${version#*.}; minor=${minor%%.*}
  (( major < 2 || (major == 2 && minor <= maximum) )) ||
    fail "${candidate#"$STAGE/"} exige $newest (max GLIBC_2.$maximum)"
}

while IFS= read -r -d '' candidate; do
  kind=$(file -b "$candidate")
  case $kind in
    *ELF*)
      relative=${candidate#"$STAGE/"}
      case $relative in
        kotor/kotor-nextos)
          [[ $kind == *"ELF 32-bit"* && $kind == *ARM* ]] ||
            fail "runtime nao e ARMHF: $kind"
          ;;
        kotor/nxextract/nxextract-ui)
          [[ $kind == *"ELF 64-bit"* && $kind == *ARM* ]] ||
            fail "UI do extrator nao e AArch64: $kind"
          ;;
        *) fail "ELF inesperado: $relative" ;;
      esac
      glibc_at_most "$candidate" 30
      ;;
    *PE32*|*Mach-O*) fail "executavel estrangeiro no pacote" ;;
  esac
done < <(find "$STAGE" -type f -print0)

[[ ! -e $STAGE/kotor/run.sh ]] || fail "run.sh voltou ao pacote"
[[ ! -e $STAGE/kotor/libkotor_input-universal.so ]] ||
  fail "normalizador separado voltou ao pacote"
[[ $(find "$STAGE" -type f -name 'kotor-*' -perm /111 | wc -l) -eq 1 ]] ||
  fail "o pacote deve conter um unico runtime KOTOR"

for script in "$STAGE/Star Wars KOTOR.sh" "$STAGE/kotor/port-env.sh" \
              "$STAGE/kotor/nxextract/nxextract-runtime-env.sh" \
              "$STAGE/kotor/nxextract/run-extractor.sh"; do
  bash -n "$script"
done

launcher_lines=$(wc -l < "$STAGE/Star Wars KOTOR.sh")
(( launcher_lines <= 500 )) ||
  fail "launcher visivel cresceu ($launcher_lines linhas): padrao e pequeno"

if grep -IRnE '(^|[[:space:]])(setsid|systemctl[[:space:]]+(stop|mask|restart)|pkill)([[:space:]]|$)' \
    "$STAGE/Star Wars KOTOR.sh" "$STAGE/kotor/port-env.sh"; then
  fail "launcher contem ciclo de vida proibido"
fi
if grep -IRnE '^[[:space:]]*(export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER=' \
    "$STAGE/Star Wars KOTOR.sh" "$STAGE/kotor/port-env.sh"; then
  fail "launcher fixou backend SDL"
fi

if find "$STAGE" \( -iname '*.apk' -o -iname '*.apkm' -o -iname '*.apks' -o \
     -iname '*.xapk' -o -iname '*.obb' -o -iname '*.sav' -o \
     -name 'libKOTOR.so' -o -name 'libandroid_port.so' -o -name 'libfmod.so' \
   \) -print -quit | grep -q .; then
  fail "dado proprietario entrou no pacote"
fi
if find "$STAGE" \( -iname '*.log' -o -iname '*.pcm' -o -iname '*.raw' -o \
     -name '__pycache__' -o -name '*.pyc' -o -name 'run.sh' \) \
     -print -quit | grep -q .; then
  fail "artefato antigo/pessoal entrou no pacote"
fi
if grep -IRnE '192[.]168[.]|169[.]254[.]|10[.][0-9]+[.]|/home/|root@|[A-Za-z0-9._%+-]+@(gmail|hotmail|outlook|yahoo|proton)[.]' \
    "$STAGE" --include='*.sh' --include='*.md' --include='*.txt' \
    --include='*.json' --include='*.py'; then
  fail "release contem IP, host ou caminho pessoal"
fi

(
  cd "$STAGE"
  find . -type f ! -path './kotor/PACKAGE-MANIFEST.sha256' -printf '%P\n' |
    sort | while IFS= read -r relative; do sha256sum -- "$relative"; done
) > "$STAGE/kotor/PACKAGE-MANIFEST.sha256"

find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(
  cd "$STAGE"
  find . -type f -printf '%P\n' | sort | zip -X -9 -q "$TMP_ZIP" -@
)
unzip -tq "$TMP_ZIP" >/dev/null
VERIFY=$TMP_ROOT/verify
mkdir -p "$VERIFY"
unzip -q "$TMP_ZIP" -d "$VERIFY"
(cd "$VERIFY" && sha256sum -c kotor/PACKAGE-MANIFEST.sha256 >/dev/null)

mkdir -p "$(dirname -- "$OUTPUT")"
install -m 0644 "$TMP_ZIP" "$OUTPUT"
(cd "$(dirname -- "$OUTPUT")" &&
 sha256sum "$(basename -- "$OUTPUT")" > "$(basename -- "$OUTPUT").sha256")

printf 'PACKAGE OK: %s\n' "$OUTPUT"
printf 'runtime: kotor-nextos | GLIBC <= 2.30 | run.sh=absent\n'
sha256sum "$BINARY" "$OUTPUT"
