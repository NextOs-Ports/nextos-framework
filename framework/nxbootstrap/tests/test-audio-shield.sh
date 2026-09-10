#!/usr/bin/env bash
# Regressao do escudo de audio do guest (E1, nxbootstrap 0.6.21+).
#
# Caso de campo: Tightrope mudo no AmberELEC porque a CFW shipa
# /etc/openal/alsoft.conf com drivers=alsa e o OpenAL EMBUTIDO (Android) do
# jogo le esse arquivo do host. O launcher pina ALSOFT_DRIVERS=opensl quando o
# port declara a capability audio.embedded-openal.
#
# O primeiro escudo NUNCA disparava: NXCOMPAT_REQUIRED_CAPABILITIES e uma lista
# separada por NEWLINE e o `case` casava por espacos (pego no A/B real no .137).
# Este teste executa o bloco REAL do launcher gerado com a lista em AMBOS os
# formatos e prova: capability presente -> ALSOFT_DRIVERS=opensl; ausente ->
# env intocado. Sem hardware, sem rede.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
ROOT=$(cd -- "$HERE/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nx-audio-shield.XXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT

fail() { printf 'audio-shield test FAIL: %s\n' "$1" >&2; exit 1; }

# 1. Gerar um launcher REAL a partir do exemplo + capability nova.
python3 - "$ROOT/examples/nxport.example.json" "$TEST_ROOT/nxport.json" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
caps = manifest.setdefault("required_capabilities", [])
if "audio.embedded-openal" not in caps:
    caps.append("audio.embedded-openal")
json.dump(manifest, open(sys.argv[2], "w"), indent=2, ensure_ascii=False)
PY
python3 -B "$ROOT/tools/generate-port.py" "$TEST_ROOT/nxport.json" \
  --output "$TEST_ROOT/render" >/dev/null
LAUNCHER=$(find "$TEST_ROOT/render" -maxdepth 1 -name '*.sh' | head -n1)
[ -n "$LAUNCHER" ] || fail "generator did not render a launcher"

# 2. Extrair o bloco real do escudo (marcadores estaveis do template).
SHIELD=$TEST_ROOT/shield.sh
sed -n '/# Guest audio shield/,/^unset NXBOOTSTRAP_CAP$/p' "$LAUNCHER" > "$SHIELD"
grep -q 'ALSOFT_DRIVERS=opensl' "$SHIELD" || \
  fail "generated launcher lacks the audio shield block"
grep -q 'for NXBOOTSTRAP_CAP in' "$SHIELD" || \
  fail "shield is not the word-splitting form (newline regression risk)"

run_shield() { # $1 = valor da lista; imprime o ALSOFT_DRIVERS resultante
  env -u ALSOFT_DRIVERS bash -c '
    NXCOMPAT_REQUIRED_CAPABILITIES=$1
    source "$2" >/dev/null
    printf "%s" "${ALSOFT_DRIVERS:-UNSET}"
  ' shield "$1" "$SHIELD"
}

NL=$'\n'
# 3. Lista NEWLINE (formato real do generator) -> pina.
[ "$(run_shield "host.portmaster${NL}audio.embedded-openal${NL}graphics.gles2")" = opensl ] || \
  fail "newline-separated capability list did not pin ALSOFT_DRIVERS (field bug back)"
# 4. Lista por ESPACO -> tambem pina.
[ "$(run_shield "host.portmaster audio.embedded-openal graphics.gles2")" = opensl ] || \
  fail "space-separated capability list did not pin ALSOFT_DRIVERS"
# 5. SEM a capability -> env intocado (zero regressao nos outros ports).
[ "$(run_shield "host.portmaster${NL}graphics.gles2")" = UNSET ] || \
  fail "shield fired without the capability (would force env on every port)"
# 6. Nome parecido NAO casa (match exato por palavra).
[ "$(run_shield "audio.embedded-openal-extra")" = UNSET ] || \
  fail "shield matched a superstring capability name"

echo "nxbootstrap audio-shield test: PASS cases=4 (newline, space, absent, superstring)"
