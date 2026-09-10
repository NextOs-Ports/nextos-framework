#!/usr/bin/env bash
# Build, host-test and atomically bundle the validated BYO-data release.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$PORT_DIR/../.." && pwd -P)
# NXRelease is the internal NextOS release tool; it is not distributed with
# this repository. Point NXRELEASE at a checkout to package a release.
NXRELEASE=${NXRELEASE:?NXRELEASE must point at the internal nxrelease.py (tool not distributed)}
NXRELEASE_VERSION=0.2.5
NXRELEASE_SHA256=097ef954261d7e31fb4a759caf2ebda9be02f069b1968e3f7b379d92f51e732f
MANIFEST="$PORT_DIR/nxrelease.json"
DESTINATION=${1:-"$PORT_DIR/.build/release"}
ARCHIVE_NAME=gunbrick.zip

fail() {
  printf 'gunbrick package error: %s\n' "$*" >&2
  exit 1
}

[[ -f $NXRELEASE && -f $MANIFEST ]] ||
  fail "canonical NXRelease or manifest is missing"
ACTUAL_SHA256=$(sha256sum -- "$NXRELEASE" | awk '{print $1}')
[[ $ACTUAL_SHA256 == "$NXRELEASE_SHA256" ]] ||
  fail "NXRelease SHA-256 drifted: $ACTUAL_SHA256"
ACTUAL_VERSION=$(python3 -B "$NXRELEASE" --version)
[[ $ACTUAL_VERSION == "nxrelease $NXRELEASE_VERSION" ]] ||
  fail "NXRelease version drifted: $ACTUAL_VERSION"
[[ ! -e $DESTINATION && ! -L $DESTINATION ]] ||
  fail "destination already exists (package outputs are never overwritten): $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/gunbrick-package.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/gunbrick-package.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *)
      printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2
      ;;
  esac
}
trap cleanup EXIT INT TERM

if [[ ${GB_SKIP_BUILD:-0} != 1 ]]; then
  "$PORT_DIR/tests/run-host.sh"
fi
python3 -B "$NXRELEASE" validate --manifest "$MANIFEST"
python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" \
  --stage "$WORK_ROOT/stage" \
  --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME" \
  --max-glibc 2.30
python3 -B "$NXRELEASE" verify \
  --archive "$DESTINATION/$ARCHIVE_NAME" \
  --sha256-file "$DESTINATION/$ARCHIVE_NAME.sha256" \
  --max-glibc 2.30

printf 'GUNBRICK BYO RELEASE PACKAGE: %s\n' "$DESTINATION/$ARCHIVE_NAME"
printf '%s\n' \
  'physical_device_evidence=1 baseline_physical_release=0.2.5 proprietary_payload=0 guest_execution=0'
sha256sum -- "$DESTINATION/$ARCHIVE_NAME"
