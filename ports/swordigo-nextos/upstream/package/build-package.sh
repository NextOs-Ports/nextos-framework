#!/usr/bin/env bash
# Build, host-test and atomically bundle the public Swordigo BYO-data release.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
VERSION=$(tr -d '\r\n' < "$PORT_DIR/version.txt")
MANIFEST="$PORT_DIR/nxrelease.json"
FRAMEWORK_ROOT=${NX_FRAMEWORK_ROOT:-"$PORT_DIR/../nextos_ports_android/framework"}
NXRELEASE=${NXRELEASE:-"$FRAMEWORK_ROOT/nxrelease/nxrelease.py"}
DESTINATION=${1:-"$PORT_DIR/.build/Swordigo.NextOS-v$VERSION-nxrelease"}
ARCHIVE_NAME="Swordigo.NextOS-v$VERSION.zip"

NXRELEASE_VERSION='nxrelease 0.2.15'
NXRELEASE_SHA256=5294ef2aafaffa892d59ee855328524a132429b20de4932aaf4ce50e76307bd1

fail() {
  printf 'swordigo package error: %s\n' "$*" >&2
  exit 1
}

case $VERSION in
  ''|*[!0-9A-Za-z._-]*) fail 'version.txt is invalid' ;;
esac
[[ -f $NXRELEASE && ! -L $NXRELEASE ]] ||
  fail "pinned NXRelease is missing or linked: $NXRELEASE"
[[ -f $MANIFEST && ! -L $MANIFEST ]] || fail 'nxrelease.json is missing or linked'
grep -Fq '"version": "'"$VERSION"'"' "$MANIFEST" ||
  fail 'version.txt and nxrelease.json package version disagree'
[[ ! -e $DESTINATION && ! -L $DESTINATION ]] ||
  fail "destination already exists (release outputs are never overwritten): $DESTINATION"

actual_tool_sha=$(sha256sum "$NXRELEASE" | awk '{print $1}')
[[ $actual_tool_sha == "$NXRELEASE_SHA256" ]] ||
  fail "NXRelease content pin mismatch: $actual_tool_sha"
actual_tool_version=$(python3 -B "$NXRELEASE" --version)
[[ $actual_tool_version == "$NXRELEASE_VERSION" ]] ||
  fail "NXRelease version mismatch: $actual_tool_version"

mkdir -p -- "$(dirname -- "$DESTINATION")"
WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/swordigo-package.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/swordigo-package.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *)
      printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2
      ;;
  esac
}
trap cleanup EXIT INT TERM

if [[ ${SWORDIGO_SKIP_BUILD:-0} != 1 ]]; then
  "$PORT_DIR/build_universal.sh"
fi
[[ -x $PORT_DIR/swordigo-nextos &&
   ! -L $PORT_DIR/swordigo-nextos ]] ||
  fail 'swordigo-nextos is missing, linked or not executable'

NXBOOTSTRAP_GENERATOR="$FRAMEWORK_ROOT/nxbootstrap/tools/generate-port.py" \
  bash "$PORT_DIR/tests/launcher_test.sh"
python3 -B "$NXRELEASE" validate --manifest "$MANIFEST"
python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" \
  --stage "$WORK_ROOT/stage" \
  --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME"
python3 -B "$NXRELEASE" verify \
  --archive "$DESTINATION/$ARCHIVE_NAME" \
  --sha256-file "$DESTINATION/$ARCHIVE_NAME.sha256"

printf 'SWORDIGO PUBLIC PACKAGE PASS: %s\n' "$DESTINATION/$ARCHIVE_NAME"
printf '%s\n' \
  "nxrelease_version=${NXRELEASE_VERSION#nxrelease }" \
  "nxrelease_sha256=$NXRELEASE_SHA256" \
  'physical_device_evidence=0' \
  'owner_data_in_package=0 device_calls=0 network_calls=0 guest_execution=0'
