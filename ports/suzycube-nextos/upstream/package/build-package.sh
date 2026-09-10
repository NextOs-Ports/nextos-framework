#!/usr/bin/env bash
# Build, host-test and atomically bundle the validated BYO-data release.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
# shellcheck source=../tools/framework-source.sh
source "$PORT_DIR/tools/framework-source.sh"
FRAMEWORK_REPOSITORY=$(sc_resolve_framework_repository "$PORT_DIR")
NXRELEASE=${SC_NXRELEASE:-"$FRAMEWORK_REPOSITORY/framework/nxrelease/nxrelease.py"}
NXRELEASE_VERSION=0.2.15
NXRELEASE_SHA256=5294ef2aafaffa892d59ee855328524a132429b20de4932aaf4ce50e76307bd1
MANIFEST="$PORT_DIR/nxrelease.json"
DESTINATION=${1:-"$PORT_DIR/.build/release"}
ARCHIVE_NAME=SuzyCube.PortMaster-v1.1.12.zip

fail() {
  printf 'suzy cube package error: %s\n' "$*" >&2
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

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/suzycube-package.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/suzycube-package.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *)
      printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2
      ;;
  esac
}
trap cleanup EXIT INT TERM

if [[ ${SC_SKIP_BUILD:-0} != 1 ]]; then
  "$PORT_DIR/tests/run-host.sh"
fi
python3 -B "$NXRELEASE" validate --manifest "$MANIFEST"
python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" \
  --stage "$WORK_ROOT/stage" \
  --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME" \
  --max-glibc 2.30
VERIFY_ARGUMENTS=(
  verify
  --archive "$DESTINATION/$ARCHIVE_NAME"
  --sha256-file "$DESTINATION/$ARCHIVE_NAME.sha256"
  --max-glibc 2.30
)
if [[ -n ${SC_PREVIOUS_ARCHIVE:-} ]]; then
  [[ -f $SC_PREVIOUS_ARCHIVE && ! -L $SC_PREVIOUS_ARCHIVE ]] ||
    fail "SC_PREVIOUS_ARCHIVE is missing, not regular, or a symlink"
  VERIFY_ARGUMENTS+=(--previous-archive "$SC_PREVIOUS_ARCHIVE")
fi
python3 -B "$NXRELEASE" "${VERIFY_ARGUMENTS[@]}"

printf 'SUZY CUBE BYO RELEASE PACKAGE: %s\n' "$DESTINATION/$ARCHIVE_NAME"
printf '%s\n' \
  'physical_device_evidence=0 baseline_physical_release=1.1.0 proprietary_payload=0 guest_execution=0'
sha256sum -- "$DESTINATION/$ARCHIVE_NAME"
