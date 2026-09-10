#!/bin/bash
# Teste isolado do contrato RestartGame do launcher publico.
set -uo pipefail

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
LAUNCHER="$TEST_DIR/../The Amazing Spider-Man 2 (Multi).sh"

fail() {
  echo "launcher_lifecycle_test: FAIL: $*" >&2
  exit 1
}

load_launcher_function() {
  local function_name=$1

  # Carrega uma implementacao real sem executar discovery, preflight ou jogo.
  # shellcheck disable=SC1090
  source <(
    awk -v signature="${function_name}()" '
      $0 == signature " {" { emit=1 }
      emit { print }
      emit && /^}/ { exit }
    ' "$LAUNCHER"
  )
}

load_launcher_function asm2_is_nextos
load_launcher_function asm2_select_binary
load_launcher_function asm2_run_foreground
declare -F asm2_is_nextos >/dev/null ||
  fail "asm2_is_nextos nao encontrado no launcher"
declare -F asm2_select_binary >/dev/null ||
  fail "asm2_select_binary nao encontrado no launcher"
declare -F asm2_run_foreground >/dev/null ||
  fail "asm2_run_foreground nao encontrado no launcher"

# A raiz de sistema injetada mantem os testes independentes do /etc e /usr
# reais da maquina que esta executando a suite.
launcher_platform_root=$(mktemp -d) ||
  fail "nao foi possivel criar fixture temporaria"
launcher_platform_game="$launcher_platform_root/game"
mkdir -p "$launcher_platform_game" "$launcher_platform_root/etc" ||
  fail "nao foi possivel criar fixture de plataforma"
trap 'rm -rf -- "$launcher_platform_root"' EXIT

GAMEDIR=$launcher_platform_game
unset CFW_NAME OS_NAME ASM2_CFW_NAME ASM2_OS_NAME
touch "$GAMEDIR/asm2_127" "$GAMEDIR/asm2_127-universal" ||
  fail "nao foi possivel criar loaders ficticios"

asm2_select_binary "$launcher_platform_root" >/dev/null
[ "$BIN" = "$GAMEDIR/asm2_127-universal" ] ||
  fail "sistema externo com ambos loaders nao selecionou universal: $BIN"

CFW_NAME=NextOS
asm2_select_binary "$launcher_platform_root" >/dev/null
[ "$BIN" = "$GAMEDIR/asm2_127" ] ||
  fail "NextOS por CFW_NAME com ambos loaders nao selecionou build atual: $BIN"
unset CFW_NAME

printf 'NAME="NextOS Retro Elite"\n' > "$launcher_platform_root/etc/os-release" ||
  fail "nao foi possivel criar os-release ficticio"
asm2_select_binary "$launcher_platform_root" >/dev/null
[ "$BIN" = "$GAMEDIR/asm2_127" ] ||
  fail "NextOS por os-release isolado nao selecionou build atual: $BIN"

rm -f "$launcher_platform_root/etc/os-release" "$GAMEDIR/asm2_127-universal"
asm2_select_binary "$launcher_platform_root" >/dev/null
[ "$BIN" = "$GAMEDIR/asm2_127" ] ||
  fail "sistema externo com apenas build NextOS nao usou fallback unico: $BIN"

touch "$GAMEDIR/asm2_127-universal" ||
  fail "nao foi possivel restaurar loader universal ficticio"
rm -f "$GAMEDIR/asm2_127"
OS_NAME="NextOS"
asm2_select_binary "$launcher_platform_root" >/dev/null
[ "$BIN" = "$GAMEDIR/asm2_127-universal" ] ||
  fail "NextOS com apenas build universal nao usou fallback unico: $BIN"
unset OS_NAME

# Consumida pela funcao de producao carregada dinamicamente acima.
# shellcheck disable=SC2034
BIN=launcher_lifecycle_fake_binary
GAMEDIR=/launcher-lifecycle-test
launcher_test_calls=0
launcher_test_sleeps=0
launcher_test_stops=0
launcher_test_mode=

sleep() {
  [ "$#" -eq 1 ] && [ "$1" = 1 ] ||
    fail "espera diferente de um segundo: $*"
  launcher_test_sleeps=$((launcher_test_sleeps + 1))
}

asm2_stop_existing() {
  launcher_test_stops=$((launcher_test_stops + 1))
}

launcher_lifecycle_fake_binary() {
  [ "$#" -eq 1 ] && [ "$1" = "$GAMEDIR" ] ||
    fail "argumentos do loader mudaram: $*"
  launcher_test_calls=$((launcher_test_calls + 1))

  case "$launcher_test_mode:$launcher_test_calls" in
    restart_then_ok:1) return 75 ;;
    restart_then_ok:2) return 0 ;;
    restart_loop:*) return 75 ;;
    ordinary:*) return 42 ;;
  esac
  return 99
}

launcher_test_mode=restart_then_ok
asm2_run_foreground
launcher_test_status=$?
[ "$launcher_test_status" -eq 0 ] ||
  fail "75 -> 0 retornou $launcher_test_status"
[ "$launcher_test_calls" -eq 2 ] ||
  fail "75 -> 0 executou $launcher_test_calls vezes"
[ "$launcher_test_sleeps" -eq 1 ] ||
  fail "75 -> 0 esperou $launcher_test_sleeps vezes"
[ "$launcher_test_stops" -eq 1 ] ||
  fail "75 -> 0 conferiu residuais $launcher_test_stops vezes"

launcher_test_mode=restart_loop
launcher_test_calls=0
launcher_test_sleeps=0
launcher_test_stops=0
asm2_run_foreground
launcher_test_status=$?
[ "$launcher_test_status" -eq 75 ] ||
  fail "75 -> 75 nao preservou rc=75 (rc=$launcher_test_status)"
[ "$launcher_test_calls" -eq 2 ] ||
  fail "anti-loop executou $launcher_test_calls vezes"
[ "$launcher_test_sleeps" -eq 1 ] ||
  fail "anti-loop esperou $launcher_test_sleeps vezes"
[ "$launcher_test_stops" -eq 1 ] ||
  fail "anti-loop conferiu residuais $launcher_test_stops vezes"

launcher_test_mode=ordinary
launcher_test_calls=0
launcher_test_sleeps=0
launcher_test_stops=0
asm2_run_foreground
launcher_test_status=$?
[ "$launcher_test_status" -eq 42 ] ||
  fail "rc comum nao foi preservado (rc=$launcher_test_status)"
[ "$launcher_test_calls" -eq 1 ] ||
  fail "rc comum causou relancamento"
[ "$launcher_test_sleeps" -eq 0 ] ||
  fail "rc comum causou espera"
[ "$launcher_test_stops" -eq 0 ] ||
  fail "rc comum acionou limpeza entre processos"

echo "launcher_lifecycle_test: PASS"
