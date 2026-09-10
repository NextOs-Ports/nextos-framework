#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Real TERM is restricted to an exact child inside the sealed PID namespace.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/../.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/run-logged-test.XXXXXX")
LOG_ROOT=$TEST_ROOT/logs
HELPER=$TEST_ROOT/helper.sh
INPUT_FILE=$TEST_ROOT/input.txt
SECRET_FILE=$TEST_ROOT/secret.txt
SECRET_VALUE=nxframework-secret-must-not-leak-48917

cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/run-logged-test.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'run-logged interruption test failed: %s\n' "$*" >&2
  exit 1
}

process_starttime() {
  local pid=$1 stat rest
  IFS= read -r stat < "/proc/$pid/stat" || return 1
  rest=${stat##*) }
  set -- $rest
  (( $# >= 20 )) || return 1
  printf '%s\n' "${20}"
}

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'sleep 1' \
  'exit 7' \
  > "$HELPER"
chmod 0755 "$HELPER"
printf 'ordinary fixture\n' > "$INPUT_FILE"
printf '%s\n' "$SECRET_VALUE" > "$SECRET_FILE"

"$PROJECT_ROOT/tools/run-logged.sh" --log-root "$LOG_ROOT" -- \
  "$HELPER" --token "$SECRET_VALUE" --password-file "$SECRET_FILE" \
  "$INPUT_FILE" &
runner_pid=$!
runner_starttime=$(process_starttime "$runner_pid") ||
  fail 'cannot capture exact runner starttime'

run_dir=
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  run_dir=$(find "$LOG_ROOT" -mindepth 1 -maxdepth 1 -type d -print -quit \
    2>/dev/null || true)
  [[ -n $run_dir && -f $run_dir/metadata.txt ]] && break
  sleep 0.05
done
[[ -n $run_dir && -f $run_dir/metadata.txt ]] ||
  fail 'runner did not create metadata in time'
[[ $(process_starttime "$runner_pid") == "$runner_starttime" ]] ||
  fail 'runner PID ownership changed before TERM'
builtin kill -TERM "$runner_pid"

set +e
wait "$runner_pid"
runner_status=$?
set -e
[[ $runner_status -eq 7 ]] ||
  fail "runner returned $runner_status instead of child status 7"

grep -F 'received_signal=TERM' "$run_dir/result.txt" >/dev/null ||
  fail 'TERM observation was not preserved'
grep -F 'command_status=7' "$run_dir/result.txt" >/dev/null ||
  fail 'child status was not preserved'
grep -F 'command_argv_sha256=' "$run_dir/metadata.txt" >/dev/null ||
  fail 'command argv digest is absent'
grep -F 'redacted' "$run_dir/metadata.txt" >/dev/null ||
  fail 'known secret arguments were not redacted'
if rg -n --fixed-strings "$SECRET_VALUE" "$run_dir" >/dev/null; then
  fail 'secret value leaked into the durable log'
fi
if rg -n --fixed-strings "$SECRET_FILE" "$run_dir" >/dev/null; then
  fail 'secret-file path leaked into the durable log'
fi
grep -F "$INPUT_FILE" "$run_dir/input-files.sha256" >/dev/null ||
  fail 'ordinary explicit input file was not hashed'
if grep -F "$SECRET_FILE" "$run_dir/input-files.sha256" >/dev/null; then
  fail 'credential file was hashed into the input manifest'
fi

(cd -- "$run_dir" && sha256sum -c MANIFEST.sha256 >/dev/null) ||
  fail 'fresh run manifest does not verify'
printf 'tamper\n' >> "$run_dir/console.log"
if (cd -- "$run_dir" && sha256sum -c MANIFEST.sha256 >/dev/null 2>&1); then
  fail 'corrupted run log still verified'
fi

printf 'run-logged interruption/redaction test passed\n'
