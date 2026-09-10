#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Run one development command while preserving a durable, self-contained log.
# This helper never sends signals, starts services, changes power/session state,
# or removes an earlier run directory.
set -uo pipefail

is_secret_option() {
  case ${1,,} in
    --password|--password-file|--token|--access-token|--refresh-token|\
    --secret|--client-secret|--api-key|--apikey|--authorization|--cookie|\
    --credential|--credentials)
      return 0
      ;;
  esac
  return 1
}

redacted_argument() {
  local argument=$1 lowered=${1,,} prefix
  case $lowered in
    --password=*|--password-file=*|--token=*|--access-token=*|\
    --refresh-token=*|--secret=*|--client-secret=*|--api-key=*|--apikey=*|\
    --authorization=*|--cookie=*|--credential=*|--credentials=*|\
    *password=*|*token=*|*secret=*|*api_key=*|*apikey=*|\
    *authorization=*|*credential=*)
      prefix=${argument%%=*}
      printf '%s=<redacted>' "$prefix"
      ;;
    *://*:*@*) printf '<redacted-uri>' ;;
    *) printf '%s' "$argument" ;;
  esac
}

usage() {
  printf 'usage: %s --log-root ABSOLUTE_DIRECTORY -- COMMAND [ARG ...]\n' \
    "${0##*/}" >&2
  exit 2
}

[[ ${1:-} == --log-root && -n ${2:-} ]] || usage
log_root=$2
shift 2
[[ ${1:-} == -- ]] || usage
shift
(( $# > 0 )) || usage

case $log_root in
  /*) ;;
  *) printf 'run-logged: log root must be absolute\n' >&2; exit 2 ;;
esac
case $log_root in
  /|/home|/home/*/..|/root)
    printf 'run-logged: refusing broad log root: %s\n' "$log_root" >&2
    exit 2
    ;;
esac

umask 077
mkdir -p -- "$log_root" || exit 1
[[ -d $log_root && ! -L $log_root && -w $log_root && -x $log_root ]] || {
  printf 'run-logged: unsafe or unwritable log root: %s\n' "$log_root" >&2
  exit 1
}

started_utc=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
run_stamp=$(date -u +%Y%m%dT%H%M%S)
run_id=${run_stamp}Z-pid$$-${RANDOM}${RANDOM}
run_dir=$log_root/$run_id
mkdir -- "$run_dir" || exit 1

metadata=$run_dir/metadata.txt
console=$run_dir/console.log
result=$run_dir/result.txt
inputs=$run_dir/input-files.sha256
command_status_record=$run_dir/command-status.txt

# Arm observation before publishing any readiness artifact. A caller that sees
# metadata.txt or console.log must never be able to hit the shell's default
# signal action in the small window between run-directory creation and the
# command wait.
received_signal=none
record_signal() {
  received_signal=$1
  printf '[run-logged] received signal %s at %s\n' \
    "$received_signal" "$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)" >> "$console"
}
trap 'record_signal HUP' HUP
trap 'record_signal INT' INT
trap 'record_signal TERM' TERM

{
  printf 'format=nxframework-run-log-v1\n'
  printf 'run_id=%s\n' "$run_id"
  printf 'started_utc=%s\n' "$started_utc"
  printf 'cwd=%s\n' "$PWD"
  printf 'runner_pid=%s\n' "$$"
  printf 'runner_ppid=%s\n' "$PPID"
  printf 'uid=%s\n' "$(id -u)"
  printf 'gid=%s\n' "$(id -g)"
  printf 'argument_count=%s\n' "$#"
  printf 'command_argv_sha256=%s\n' "$({ printf '%s\0' "$@"; } | sha256sum | awk '{print $1}')"
  printf 'command_redacted='
  redact_next=0
  for argument in "$@"; do
    if (( redact_next != 0 )); then
      printable='<redacted>'
      redact_next=0
    else
      printable=$(redacted_argument "$argument")
      is_secret_option "$argument" && redact_next=1
    fi
    printf ' %q' "$printable"
  done
  printf '\n'
  if git rev-parse --show-toplevel >/dev/null 2>&1; then
    printf 'git_root=%s\n' "$(git rev-parse --show-toplevel)"
    printf 'git_head=%s\n' "$(git rev-parse HEAD 2>/dev/null || printf unborn)"
    printf 'git_branch=%s\n' \
      "$(git symbolic-ref --quiet --short HEAD 2>/dev/null || printf detached)"
  fi
} > "$metadata"

# Hash only explicit regular-file arguments. The complete environment is not
# dumped because it may contain credentials; the exact command is sufficient
# to reproduce the invocation without leaking ambient secrets.
: > "$inputs"
skip_next=0
for argument in "$@"; do
  if (( skip_next != 0 )); then
    skip_next=0
    continue
  fi
  if is_secret_option "$argument"; then
    skip_next=1
    continue
  fi
  [[ $(redacted_argument "$argument") == "$argument" ]] || continue
  if [[ -f $argument && ! -L $argument && -r $argument ]]; then
    sha256sum -- "$argument" >> "$inputs"
  fi
done

printf '[run-logged] run_id=%s\n' "$run_id" | tee -a "$console"
printf '[run-logged] log_dir=%s\n' "$run_dir" | tee -a "$console"

set +e
set +o pipefail
(
  "$@"
  child_status=$?
  printf '%s\n' "$child_status" > "$command_status_record" || exit 125
  exit "$child_status"
) 2>&1 | tee -a "$console" &
pipeline_pid=$!
set -o pipefail

# A handled signal interrupts Bash's wait with 128+signal even though the
# asynchronous pipeline is still alive. Consult Bash's own job table (not
# /proc or kill -0) and wait for this exact job again. PID reuse cannot turn an
# unrelated host process into a wait target because wait only accepts children.
while :; do
  wait "$pipeline_pid"
  candidate_status=$?
  # This runner creates exactly one asynchronous job. Bash may report its
  # process-group leader rather than the last pipeline PID, so presence in the
  # shell job table is the stable ownership check.
  [[ -n $(jobs -pr) ]] && continue
  tee_status=$candidate_status
  break
done

# The child job is fully joined. Freeze signal observation before result and
# manifest materialization so a late HUP/INT/TERM cannot make result.txt stale
# or mutate console.log after it has been hashed. KILL remains inherently
# uncatchable; partial output from it is intentionally not a valid manifest.
trap '' HUP INT TERM
set -e

if [[ -f $command_status_record && ! -L $command_status_record ]]; then
  command_status=$(<"$command_status_record")
  case $command_status in ''|*[!0-9]*) command_status=125 ;; esac
else
  command_status=125
fi

ended_utc=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
{
  printf 'format=nxframework-run-result-v1\n'
  printf 'run_id=%s\n' "$run_id"
  printf 'ended_utc=%s\n' "$ended_utc"
  printf 'command_status=%s\n' "$command_status"
  printf 'tee_status=%s\n' "$tee_status"
  printf 'received_signal=%s\n' "$received_signal"
} > "$result"

printf '[run-logged] status=%s manifest=%s\n' \
  "$command_status" "$run_dir/MANIFEST.sha256" | tee -a "$console"

(
  cd -- "$run_dir" || exit 1
  sha256sum metadata.txt console.log result.txt input-files.sha256 \
    command-status.txt \
    > MANIFEST.sha256
)

if (( tee_status != 0 )); then
  exit 125
fi
exit "$command_status"
