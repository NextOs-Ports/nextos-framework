#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Static regression gate for the 2026-08-08 host-session incident.
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SOURCE=$PROJECT_ROOT/nxbootstrap.sh
DEFAULT_TEST=$PROJECT_ROOT/tests/test-nxbootstrap.sh
GENERATOR_TEST=$PROJECT_ROOT/tests/test-generator.sh
BEHAVIOR_TEST=$PROJECT_ROOT/tests/test-launcher-behavior.sh
ISOLATED_RUNNER=$PROJECT_ROOT/tests/run-isolated.sh
GENERATED_WRAPPER_SOURCES=(
  "$PROJECT_ROOT/templates"
  "$PROJECT_ROOT/tools/generate-port.py"
)

fail() {
  printf 'nxbootstrap safety gate failed: %s\n' "$*" >&2
  exit 1
}

for removed_name in \
  nxbootstrap_name_matches \
  nxbootstrap_process_has_port_identity \
  nxbootstrap_process_matches \
  nxbootstrap_matching_processes \
  nxbootstrap_collect_process_tree \
  nxbootstrap_live_tracked_entries \
  nxbootstrap_signal_tracked_entries \
  nxbootstrap_sweep_verified_instances \
  nxbootstrap_sweep_old_instance; do
  if rg -n --fixed-strings "$removed_name" "$SOURCE" >/dev/null; then
    fail "removed host-process matcher returned: $removed_name"
  fi
done

if rg -n '^[[:space:]]*for[[:space:]].*\[0-9\]\*' "$SOURCE" >/dev/null; then
  fail 'production bootstrap enumerates the host PID namespace'
fi

if rg -n 'NXBOOTSTRAP_SKIP_PROCESS_SWEEP|NXBOOTSTRAP_SWEEP_' \
    "$SOURCE" "$DEFAULT_TEST" "$GENERATOR_TEST" \
    "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" \
    "$PROJECT_ROOT/README.md" >/dev/null; then
  fail 'obsolete process-sweep controls remain in nxbootstrap'
fi

if rg -n '(^|[^[:alnum:]_])(pkill|killall|systemctl|loginctl|shutdown|reboot|poweroff)([^[:alnum:]_]|$)' \
    "$SOURCE" "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" >/dev/null; then
  fail 'production bootstrap contains a broad process/session/system command'
fi

if rg -n 'eval[[:space:]]' \
    "$SOURCE" "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" >/dev/null; then
  fail 'production bootstrap executes an eval command'
fi
# $ESUDO is the canonical PortMaster privilege helper supplied by
# control.txt; the generated launcher may use it (chmod of exec bits and
# TTY/uinput nodes, the fleet-proven pattern), and the generator embeds
# those launcher blocks. The retired library still must not.
if rg -n '\$\{?ESUDO' "$SOURCE" >/dev/null; then
  fail 'production bootstrap executes an ambient privilege command'
fi

mapfile -t kill_lines < <(
  rg -n '^[[:space:]]*(builtin[[:space:]]+)?kill[[:space:]]' "$SOURCE"
)
[[ ${#kill_lines[@]} -eq 1 ]] ||
  fail "unexpected number of production kill invocations: ${#kill_lines[@]}"
[[ ${kill_lines[0]} == *'builtin kill -"$signal" "$pid"'* ]] ||
  fail 'the only real signal path is not the exact-child helper'

if rg -n '^[[:space:]]*(builtin[[:space:]]+)?kill[[:space:]]' \
    "$DEFAULT_TEST" "$GENERATOR_TEST" >/dev/null; then
  fail 'the default test suite can send a process signal'
fi

for guarded_test in "$DEFAULT_TEST" "$GENERATOR_TEST"; do
  if ! rg -n 'nxbootstrap_require_private_pid_namespace' \
      "$guarded_test" >/dev/null; then
    fail "process-bearing test lacks the namespace guard: $guarded_test"
  fi
done

if ! rg -n --fixed-strings \
    'unshare --user --map-root-user --pid --fork --kill-child=KILL' \
    "$ISOLATED_RUNNER" >/dev/null; then
  fail 'isolated runner does not create the required private namespaces'
fi

for required_token in \
  "trap 'nxbootstrap_on_signal 129' HUP" \
  'NXBOOTSTRAP_CLEANED=1' \
  'nxbootstrap_open_fresh_log_fd' \
  "stat -L -c '%d:%i'" \
  'nxbootstrap_file_link_count' \
  'nxbootstrap_validate_elf_contract' \
  "'libSDL2-2.0.so*'"; do
  rg -n --fixed-strings "$required_token" "$SOURCE" >/dev/null ||
    fail "mandatory adversarial contract is missing: $required_token"
done

# 0.6.16 launcher safety tokens: complete PortMaster discovery, guarded
# port-env.sh sourcing, portable
# single-instance lock, owner-only pre-runtime diagnostics, verified dialog
# handoff, signal-forwarding cleanup and console reset before pm_finish.
for template_token in \
  '"/storage/.config/PortMaster"' \
  '[ -f "$NXBOOTSTRAP_PM_CANDIDATE/control.txt" ]' \
  '@PORT_ID@-launcher-error.$$.log' \
  'NXBOOTSTRAP_EARLY_LOG_ACTIVE=1' \
  'nxbootstrap_observe_cfw_name' \
  'arkos4clone-uboot.dtb' \
  'nxbootstrap_install_exit_trap' \
  'NXBOOTSTRAP_DIALOG_PIPE=${PM_PIPE:-}' \
  'declare -F PortMasterDialogExit' \
  'PM_PIPE remained after close request' \
  '@NXSPLASH_BLOCK@' \
  '"$GAMEDIR/nxsplash-nextos"' \
  '[ ! -L "$GAMEDIR/port-env.sh" ]' \
  'flock -n 9' \
  'NXBOOTSTRAP_FALLBACK_LOCK_ACQUIRED=1' \
  'pid=$$ token=$NXBOOTSTRAP_FALLBACK_LOCK_TOKEN' \
  'rmdir "$NXBOOTSTRAP_FALLBACK_LOCK_DIR"' \
  'support-bundle=share only the sanitized nxobs bundle or its manifest hash; never raw logs' \
  'command ls -Lldn /proc/self/fd/9' \
  '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9' \
  "trap '' INT TERM HUP" \
  "printf '\\033c'"; do
  # The visible wrapper is composed by the templates and generate-port.py.
  # A token moving between those two sources must not make this gate blind.
  rg -n --fixed-strings "$template_token" \
      "${GENERATED_WRAPPER_SOURCES[@]}" >/dev/null ||
    fail "generated wrapper safety contract is missing: $template_token"
done

if rg -n 'files-to-send=|send[^\n]*(log\.txt|events\.jsonl|nxextract\.log)' \
    "${GENERATED_WRAPPER_SOURCES[@]}" >/dev/null; then
  fail 'generated wrapper still asks users to share raw logs'
fi

if rg -n '(^|[;&|[:space:]])stat[[:space:]]+-' \
    "${GENERATED_WRAPPER_SOURCES[@]}" >/dev/null; then
  fail 'generated wrapper still requires the external stat command'
fi

# These field regressions are permanent release floors, not version-local
# examples. A later bootstrap cannot silently delete either executable proof.
for cumulative_gate in \
  'NXBEHAV_PATH="$TEST_ROOT/no-stat:$PATH"' \
  'launcher called stat or did not launch the child' \
  'NXBEHAV_DIALOG_MODE=close' \
  'close API unavailable while PM_PIPE is active' \
  'PM_PIPE remained after close request'; do
  rg -n --fixed-strings "$cumulative_gate" "$BEHAVIOR_TEST" >/dev/null ||
    fail "cumulative launcher regression gate is missing: $cumulative_gate"
done

for fallback_gate in \
  'NXBEHAV_BASH_ENV=$NO_FLOCK_BASH_ENV' \
  'second no-flock instance deleted the first lock' \
  'fallback lock with a changed owner was released'; do
  rg -n --fixed-strings "$fallback_gate" "$BEHAVIOR_TEST" >/dev/null ||
    fail "no-flock ownership regression gate is missing: $fallback_gate"
done

if rg -n --fixed-strings \
    'rm -rf "$NXBOOTSTRAP_FALLBACK_LOCK_DIR"' \
    "${GENERATED_WRAPPER_SOURCES[@]}" >/dev/null; then
  fail 'no-flock cleanup can recursively remove an unowned lock'
fi

if ! rg -n '^NXBOOTSTRAP_PROC_ROOT=/proc$' "$SOURCE" >/dev/null; then
  fail 'production procfs root is not reset to /proc while sourcing'
fi

if rg -n '\$\{NXBOOTSTRAP_PROC_ROOT:-' "$SOURCE" >/dev/null; then
  fail 'production still accepts an ambient procfs fallback expression'
fi

(
  # Sourcing defines functions only; the subshell prevents state from escaping.
  # Exercise the runtime path, not just the mirrored text checked by Python.
  # shellcheck disable=SC1090
  source "$SOURCE"
  nxbootstrap_capability_known host.portmaster
  nxbootstrap_validate_named_list capability \
    $'host.portmaster\ngraphics.gles2\ninput.controller-api'
  if nxbootstrap_capability_known host.unregistered-capability ||
     nxbootstrap_validate_named_list capability host.rocknix ||
     nxbootstrap_validate_named_list capability host.muos ||
     nxbootstrap_validate_named_list capability host.personal-name ||
     nxbootstrap_validate_named_list capability host.ipv4-address; then
    exit 1
  fi
) || fail 'runtime capability allowlist accepted an unknown/identity name'

# Um bash -n POR arquivo: a invocacao unica so' analisava o primeiro e os
# demais viravam parametros posicionais (mesma classe do audit geral, 23/08).
for NX_SYNTAX_FILE in "$SOURCE" "$DEFAULT_TEST" "$GENERATOR_TEST" \
  "$PROJECT_ROOT/tests/private-pid-namespace.sh" \
  "$PROJECT_ROOT/tests/isolated-suite.sh" \
  "$PROJECT_ROOT/tests/test-runner-interruption.sh" \
  "$PROJECT_ROOT/tests/test-namespace-watchdog.sh" \
  "$ISOLATED_RUNNER" "$PROJECT_ROOT/templates/launcher.sh.in" "$0"; do
  bash -n "$NX_SYNTAX_FILE" || fail "shell syntax: $NX_SYNTAX_FILE"
done

# As raizes PortMaster do launcher PUBLICADO tem de ser o espelho exato da
# tabela da biblioteca (0.5.1 conhecia 33; o launcher publicado tinha 22 e
# Miyoo/spruce, RetroDECK e "Ports" maiusculo perdiam controles). Comparacao
# por conjunto das entradas ESTATICAS (comecam com /); as dinamicas (dirname
# do $0 / XDG / HOME) existem nos dois lados por construcao.
NX_TEMPLATE_ROOTS=$(sed -n '/^NXBOOTSTRAP_PM_ROOTS=(/,/^)/p' \
    "$PROJECT_ROOT/templates/launcher.sh.in" |
  tr ' ' '\n' | tr -d '"' | LC_ALL=C grep '^/' | LC_ALL=C sort -u)
NX_LIBRARY_ROOTS=$(sed -n '/^nxbootstrap_default_portmaster_roots()/,/^}/p' \
    "$SOURCE" |
  tr ' \t' '\n\n' | tr -d '"\\' | LC_ALL=C grep '^/' | LC_ALL=C sort -u)
[ -n "$NX_TEMPLATE_ROOTS" ] && [ -n "$NX_LIBRARY_ROOTS" ] || \
  fail 'PortMaster root tables could not be parsed'
if [ "$NX_TEMPLATE_ROOTS" != "$NX_LIBRARY_ROOTS" ]; then
  printf 'template-only roots:\n%s\n' \
    "$(LC_ALL=C comm -23 <(printf '%s\n' "$NX_TEMPLATE_ROOTS") \
       <(printf '%s\n' "$NX_LIBRARY_ROOTS"))" >&2
  printf 'library-only roots:\n%s\n' \
    "$(LC_ALL=C comm -13 <(printf '%s\n' "$NX_TEMPLATE_ROOTS") \
       <(printf '%s\n' "$NX_LIBRARY_ROOTS"))" >&2
  fail 'PortMaster root tables diverged between launcher template and library'
fi
printf 'nxbootstrap static safety gate passed\n'
