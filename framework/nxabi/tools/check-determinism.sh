#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M17-019: run one build command twice, in two distinct owned work trees, and
# require byte-identical outputs.  The command never runs on the host tree and
# never touches the repository.
#
# usage:
#   check-determinism.sh --artifact NAME [--artifact NAME ...] -- COMMAND [ARGS...]
#
# The command runs with CWD set to a fresh mktemp directory and receives
# NX_SOURCE_ROOT (the repository) and SOURCE_DATE_EPOCH in the environment.
set -euo pipefail
export LC_ALL=C

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
ARTIFACTS=()
WORK_ROOT=${TMPDIR:-/tmp}

fail() {
  printf 'check-determinism: FAIL %s\n' "$*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --artifact)
      [[ $# -ge 2 ]] || fail "--artifact needs a value"
      ARTIFACTS+=("$2")
      shift 2
      ;;
    --)
      shift
      break
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

[[ ${#ARTIFACTS[@]} -gt 0 ]] || fail "no --artifact given"
[[ $# -gt 0 ]] || fail "no build command given"

for artifact in "${ARTIFACTS[@]}"; do
  case $artifact in
    /*|*..*) fail "artifact must be a simple relative path: $artifact" ;;
  esac
done

WORK_A=$(mktemp -d "$WORK_ROOT/nxabi-determinism-a.XXXXXX")
WORK_B=$(mktemp -d "$WORK_ROOT/nxabi-determinism-b.XXXXXX")

cleanup() {
  local status=$?
  trap - EXIT
  for tree in "$WORK_A" "$WORK_B"; do
    case $tree in
      "$WORK_ROOT"/nxabi-determinism-?.??????)
        chmod -R u+w -- "$tree" 2>/dev/null || true
        rm -rf -- "$tree"
        ;;
      *)
        printf 'check-determinism: refused cleanup outside owned mktemp: %s\n' \
          "$tree" >&2
        status=1
        ;;
    esac
  done
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

run_once() {
  local tree=$1
  shift
  ( cd -- "$tree" && \
    NX_SOURCE_ROOT="$REPO_ROOT" \
    SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1600000000}" \
    "$@" ) || fail "build command failed in $tree"
}

printf 'check-determinism: pass 1\n'
run_once "$WORK_A" "$@"
printf 'check-determinism: pass 2\n'
run_once "$WORK_B" "$@"

status=0
for artifact in "${ARTIFACTS[@]}"; do
  a="$WORK_A/$artifact"
  b="$WORK_B/$artifact"
  [[ -f $a ]] || fail "pass 1 did not produce $artifact"
  [[ -f $b ]] || fail "pass 2 did not produce $artifact"
  hash_a=$(sha256sum -- "$a" | cut -d' ' -f1)
  hash_b=$(sha256sum -- "$b" | cut -d' ' -f1)
  if [[ $hash_a == "$hash_b" ]]; then
    printf 'check-determinism: OK   %s %s\n' "$artifact" "$hash_a"
  else
    printf 'check-determinism: DIFF %s\n  pass1=%s\n  pass2=%s\n' \
      "$artifact" "$hash_a" "$hash_b" >&2
    status=1
  fi
done

[[ $status -eq 0 ]] || fail "at least one artifact is not reproducible"
printf 'check-determinism: PASS artifacts=%d\n' "${#ARTIFACTS[@]}"
