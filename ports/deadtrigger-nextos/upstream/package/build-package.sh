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
NXRELEASE="$REPO_ROOT/framework/nxrelease/nxrelease.py"
NXRELEASE_VERSION=0.2.5
NXRELEASE_SHA256=097ef954261d7e31fb4a759caf2ebda9be02f069b1968e3f7b379d92f51e732f
MANIFEST="$PORT_DIR/nxrelease.json"
DESTINATION=${1:-"$PORT_DIR/.build/release"}
ARCHIVE_NAME=DeadTrigger.NextOS-v1.1.1-FINAL.zip

fail() { printf 'deadtrigger package error: %s\n' "$*" >&2; exit 1; }

[[ -f $NXRELEASE && -f $MANIFEST ]] || fail "release tool/manifest missing"
[[ $(sha256sum -- "$NXRELEASE" | awk '{print $1}') == "$NXRELEASE_SHA256" ]] ||
  fail "NXRelease SHA-256 drifted"
[[ $(python3 -B "$NXRELEASE" --version) == "nxrelease $NXRELEASE_VERSION" ]] ||
  fail "NXRelease version drifted"
[[ ! -e $DESTINATION && ! -L $DESTINATION ]] ||
  fail "destination already exists: $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/deadtrigger-package.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/deadtrigger-package.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *) printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT INT TERM

if [[ ${DT_SKIP_BUILD:-0} != 1 ]]; then
  "$PORT_DIR/tests/run-host.sh"
fi
python3 -B "$NXRELEASE" validate --manifest "$MANIFEST"
python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" --stage "$WORK_ROOT/stage" \
  --destination "$DESTINATION" --archive-name "$ARCHIVE_NAME" \
  --max-glibc 2.30
python3 -B "$NXRELEASE" verify \
  --archive "$DESTINATION/$ARCHIVE_NAME" \
  --sha256-file "$DESTINATION/$ARCHIVE_NAME.sha256" --max-glibc 2.30

printf 'DEAD TRIGGER BYO RELEASE PACKAGE: %s\n' "$DESTINATION/$ARCHIVE_NAME"
printf '%s\n' 'physical_device_evidence=1 proprietary_payload=0 guest_execution=0'
sha256sum -- "$DESTINATION/$ARCHIVE_NAME"
