#!/usr/bin/env bash
# Build, gate and bundle the public Terraria BYO-data release.
# Generated from framework/nxrelease/templates/build-package.sh.in.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:-${NX_FRAMEWORK_ROOT:-}}
if [[ -z $FRAMEWORK_ROOT ]]; then
  for candidate in "$PORT_DIR/../../framework" \
                   "$PORT_DIR/../nextos_ports_android/framework"; do
    if [[ -f $candidate/nxbootstrap/VERSION ]]; then
      FRAMEWORK_ROOT=$(CDPATH= cd -- "$candidate" && pwd -P)
      break
    fi
  done
fi
[[ -n $FRAMEWORK_ROOT && -f $FRAMEWORK_ROOT/nxbootstrap/VERSION ]] || {
  printf 'set NEXTOS_FRAMEWORK_ROOT to the pinned NextOS framework tree\n' >&2
  exit 1
}

NXRELEASE="$FRAMEWORK_ROOT/nxrelease/nxrelease.py"
RENDER="$FRAMEWORK_ROOT/nxrelease/nx-render-manifest.py"
MANIFEST="$PORT_DIR/nxrelease.json"
PORT_ID=terraria
SOURCE_URL=https://github.com/NextOs-Ports/terraria-nextos
MAX_GLIBC=2.30

fail() {
  printf '%s package error: %s\n' "$PORT_ID" "$*" >&2
  exit 1
}

[[ -f $NXRELEASE ]] || fail "release tool missing: $NXRELEASE"
[[ -f $RENDER ]] || fail "manifest renderer missing: $RENDER"

# Always rebuild: a source fix must never ship with yesterday's loader bytes.
if [[ ${NX_SKIP_BUILD:-0} != 1 ]]; then
  if [[ -x $PORT_DIR/build.sh ]]; then
    (cd "$PORT_DIR" && ./build.sh) || fail 'loader build failed'
  elif [[ -x $PORT_DIR/build_universal.sh ]]; then
    (cd "$PORT_DIR" && ./build_universal.sh) || fail 'loader build failed'
  else
    fail 'no build.sh or build_universal.sh'
  fi
fi

python3 -B "$RENDER" --port-dir "$PORT_DIR" \
  --framework-root "$FRAMEWORK_ROOT" --source-url "$SOURCE_URL" \
  --max-glibc "$MAX_GLIBC" || fail 'manifest render failed'

python3 -B "$NXRELEASE" validate --manifest "$MANIFEST" \
  --max-glibc "$MAX_GLIBC" || fail 'manifest validation failed'

VERSION=$(python3 -c \
  'import json,sys;print(json.load(open(sys.argv[1]))["package"]["version"])' \
  "$MANIFEST")
DESTINATION=${1:-"$PORT_DIR/.build/$PORT_ID-$VERSION-nxrelease"}
ARCHIVE_NAME="$PORT_ID.zip"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/$PORT_ID-package.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT INT TERM
[[ ! -e $DESTINATION ]] ||
  fail "destination already exists (release outputs are never overwritten): $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

python3 -B "$NXRELEASE" bundle --manifest "$MANIFEST" \
  --stage "$STAGE/stage" --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME" --max-glibc "$MAX_GLIBC" ||
  fail 'bundle failed'

ARCHIVE="$DESTINATION/$ARCHIVE_NAME"
[[ -f $ARCHIVE ]] || fail "archive not produced: $ARCHIVE"

python3 -B "$NXRELEASE" verify --archive "$ARCHIVE" \
  --max-glibc "$MAX_GLIBC" || fail 'archive verification failed'
python3 -B "$FRAMEWORK_ROOT/tests/audit-portmaster-zip.py" "$ARCHIVE" ||
  fail 'PortMaster zip audit failed'

sha256sum "$ARCHIVE" | awk '{print $1"  '"$ARCHIVE_NAME"'"}' \
  > "$ARCHIVE.sha256"
printf '%s PUBLIC PACKAGE PASS: %s\n' "$PORT_ID" "$ARCHIVE"
sha256sum "$ARCHIVE"
