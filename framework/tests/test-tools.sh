#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Filesystem-only tests for durable logging and checkpoint restoration.
set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
RUN_LOGGED=$REPO_ROOT/framework/tools/run-logged.sh
CAPTURE=$REPO_ROOT/framework/tools/capture-checkpoint.sh
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/framework-tools-test.XXXXXX")

cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/framework-tools-test.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'framework tools test failed: %s\n' "$*" >&2
  exit 1
}

LOG_ROOT=$TEST_ROOT/logs
INPUT_FILE=$TEST_ROOT/input.txt
SECRET_FILE=$TEST_ROOT/credential.txt
SECRET_VALUE=framework-tool-secret-72913
printf 'ordinary input\n' > "$INPUT_FILE"
printf '%s\n' "$SECRET_VALUE" > "$SECRET_FILE"

set +e
"$RUN_LOGGED" --log-root "$LOG_ROOT" -- \
  bash -c 'exit 9' _ --token "$SECRET_VALUE" \
  --password-file "$SECRET_FILE" "$INPUT_FILE" >/dev/null
logged_status=$?
set -e
[[ $logged_status -eq 9 ]] || fail 'run-logged did not preserve status 9'
run_dir=$(find "$LOG_ROOT" -mindepth 1 -maxdepth 1 -type d -print -quit)
[[ -n $run_dir ]] || fail 'run-logged created no run directory'
(cd -- "$run_dir" && sha256sum -c MANIFEST.sha256 >/dev/null) ||
  fail 'run-logged manifest does not verify'
grep -F 'command_status=9' "$run_dir/result.txt" >/dev/null ||
  fail 'run-logged result omitted child status'
grep -F 'command_argv_sha256=' "$run_dir/metadata.txt" >/dev/null ||
  fail 'run-logged metadata omitted argv digest'
if rg -n --fixed-strings "$SECRET_VALUE" "$run_dir" >/dev/null; then
  fail 'run-logged leaked a known secret value'
fi
if rg -n --fixed-strings "$SECRET_FILE" "$run_dir" >/dev/null; then
  fail 'run-logged leaked a credential-file path'
fi

set +e
"$RUN_LOGGED" --log-root / -- true >/dev/null 2>&1
broad_log_status=$?
set -e
[[ $broad_log_status -eq 2 ]] ||
  fail 'run-logged accepted the filesystem root as log root'

FIXTURE_REPO=$TEST_ROOT/repository
CHECKPOINT_ROOT=$TEST_ROOT/checkpoints
RESTORE_ROOT=$TEST_ROOT/restore
mkdir -p "$FIXTURE_REPO/scope/sub"
git init -q "$FIXTURE_REPO"
printf 'tracked baseline\n' > "$FIXTURE_REPO/scope/tracked.txt"
(
  cd -- "$FIXTURE_REPO"
  git add scope/tracked.txt
  git -c user.name=Fixture -c user.email=fixture.invalid commit -qm baseline
)
printf 'tracked modified\n' > "$FIXTURE_REPO/scope/tracked.txt"
printf 'untracked payload\n' > "$FIXTURE_REPO/scope/sub/untracked.txt"
ln -s sub/untracked.txt "$FIXTURE_REPO/scope/link.txt"

checkpoint_dir=$(
  "$CAPTURE" --repo "$FIXTURE_REPO" \
    --checkpoint-root "$CHECKPOINT_ROOT" -- scope
)
[[ -d $checkpoint_dir ]] || fail 'capture-checkpoint returned no directory'
(cd -- "$checkpoint_dir" && sha256sum -c MANIFEST.sha256 >/dev/null) ||
  fail 'checkpoint manifest does not verify'
grep -F 'included_path=scope' "$checkpoint_dir/metadata.txt" >/dev/null ||
  fail 'checkpoint metadata omitted its exact scope'
grep -F 'scope/sub/untracked.txt' "$checkpoint_dir/source-files.sha256" >/dev/null ||
  fail 'checkpoint omitted an untracked source file'
mkdir -p "$RESTORE_ROOT"
tar -xzf "$checkpoint_dir/source-snapshot.tar.gz" -C "$RESTORE_ROOT"
diff -ru --no-dereference "$FIXTURE_REPO/scope" "$RESTORE_ROOT/scope" >/dev/null ||
  fail 'restored checkpoint differs from its source snapshot'
[[ -L $RESTORE_ROOT/scope/link.txt &&
   $(readlink "$RESTORE_ROOT/scope/link.txt") == sub/untracked.txt ]] ||
  fail 'checkpoint did not preserve a contained symlink'

second_checkpoint=$(
  "$CAPTURE" --repo "$FIXTURE_REPO" \
    --checkpoint-root "$CHECKPOINT_ROOT" -- scope
)
[[ $second_checkpoint != "$checkpoint_dir" && -d $checkpoint_dir &&
   -d $second_checkpoint ]] ||
  fail 'a later checkpoint replaced an earlier checkpoint'

set +e
"$CAPTURE" --repo "$FIXTURE_REPO" --checkpoint-root / -- scope \
  >/dev/null 2>&1
broad_checkpoint_status=$?
set -e
[[ $broad_checkpoint_status -eq 2 ]] ||
  fail 'capture-checkpoint accepted the filesystem root'

printf 'framework logging/checkpoint restoration tests passed\n'
