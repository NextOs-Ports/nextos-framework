#!/usr/bin/env bash
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sdv-launcher-test.XXXXXX")
cleanup() {
  rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM

fail() {
  printf 'launcher wrapper test failed: %s\n' "$*" >&2
  exit 1
}

ROM_ROOT="$TMP_ROOT/custom-roms"
mkdir -p "$ROM_ROOT/ports/sdvnextos" "$ROM_ROOT/ports_scripts"
cp -- "$REPO/arkos/Stardew Valley (NextOS).sh" \
  "$ROM_ROOT/ports/Stardew Valley (NextOS).sh"
cp -- "$REPO/arkos/Stardew Valley (NextOS).sh" \
  "$ROM_ROOT/ports_scripts/Stardew Valley (NextOS).sh"

cat > "$ROM_ROOT/ports/sdvnextos/run.sh" <<'SH'
#!/usr/bin/env bash
HELPER_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
printf '%s\n' "$HELPER_DIR/run.sh" "$#" "$@"
exit 37
SH
chmod +x "$ROM_ROOT/ports/sdvnextos/run.sh"

for entry in \
  "$ROM_ROOT/ports/Stardew Valley (NextOS).sh" \
  "$ROM_ROOT/ports_scripts/Stardew Valley (NextOS).sh"
do
  set +e
  output=$(cd / && sh "$entry" 'first argument' second)
  status=$?
  set -e
  [ "$status" -eq 37 ] || fail "exit status was not propagated by $entry"
  expected=$(printf '%s\n' \
    "$ROM_ROOT/ports/sdvnextos/run.sh" 2 'first argument' second)
  [ "$output" = "$expected" ] || fail "target or arguments were wrong for $entry"
done

# Regressao v1.1.5 (muOS/RG 40XX-H): o arquivo visivel pode ser um symlink criado
# pelo frontend. Se `dirname $0` nao resolver o alvo real, o wrapper procura o
# run.sh no diretorio errado e o port nunca inicia.
LINK_DIR="$TMP_ROOT/atalhos"
mkdir -p "$LINK_DIR"
ln -sf "$ROM_ROOT/ports/Stardew Valley (NextOS).sh" "$LINK_DIR/Stardew.sh"
set +e
output=$(cd / && sh "$LINK_DIR/Stardew.sh" 'first argument' second)
status=$?
set -e
[ "$status" -eq 37 ] ||
  fail "wrapper reached through a symlink did not resolve the real runtime"
expected=$(printf '%s\n' \
  "$ROM_ROOT/ports/sdvnextos/run.sh" 2 'first argument' second)
[ "$output" = "$expected" ] || fail "symlinked wrapper resolved the wrong target"

# E quando o runtime realmente nao existe, o wrapper NAO pode falhar em silencio:
# o frontend descarta stderr, entao sem arquivo em disco nao ha o que reportar.
LOST_ROOT="$TMP_ROOT/sem-runtime"
mkdir -p "$LOST_ROOT"
cp -- "$REPO/arkos/Stardew Valley (NextOS).sh" "$LOST_ROOT/Stardew Valley (NextOS).sh"
set +e
( cd / && TMPDIR="$LOST_ROOT" sh "$LOST_ROOT/Stardew Valley (NextOS).sh" ) >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail "missing runtime should exit 1"
[ -s "$LOST_ROOT/stardew-launcher-error.log" ] ||
  fail "wrapper failed silently: no error log was written to disk"

printf 'launcher wrapper tests: PASS\n'
