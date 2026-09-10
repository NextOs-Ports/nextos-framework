#!/usr/bin/env bash
# Build, generate, gate and bundle the public Sally Face BYO-data release.
# The only accepted framework input is the frozen V3 RC5 integration tree.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:-${NX_FRAMEWORK_ROOT:-}}
PORT_ID=sallyface
SOURCE_URL=https://github.com/NextOs-Ports/sallyface-nextos
MAX_GLIBC=2.30
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}
EXPECTED_FRAMEWORK_COMMIT=a548d4b6982d70917ee79a0e584d45119c9dcbae

fail() { printf '%s package error: %s\n' "$PORT_ID" "$*" >&2; exit 1; }

[[ -n $FRAMEWORK_ROOT && -d $FRAMEWORK_ROOT ]] ||
  fail 'set NEXTOS_FRAMEWORK_ROOT to framework-v3-rc5-20260828/framework'
FRAMEWORK_ROOT=$(CDPATH= cd -- "$FRAMEWORK_ROOT" && pwd -P)
FRAMEWORK_REPO=$(CDPATH= cd -- "$FRAMEWORK_ROOT/.." && pwd -P)

component_version() {
  local component=$1
  [[ -f $FRAMEWORK_ROOT/$component/VERSION ]] || return 1
  tr -d '\n' < "$FRAMEWORK_ROOT/$component/VERSION"
}

[[ $(component_version nxbootstrap) == 0.6.36 ]] || fail 'nxbootstrap is not 0.6.36'
[[ $(component_version nxgenerator) == 0.2.18 ]] || fail 'nxgenerator is not 0.2.18'
[[ $(component_version nxrelease) == 0.2.39 ]] || fail 'nxrelease is not 0.2.39'
ACTUAL_FRAMEWORK_COMMIT=$(git -C "$FRAMEWORK_REPO" rev-parse HEAD 2>/dev/null) ||
  fail 'framework tree is not an immutable Git checkout'
[[ $ACTUAL_FRAMEWORK_COMMIT == $EXPECTED_FRAMEWORK_COMMIT ]] ||
  fail "framework commit is $ACTUAL_FRAMEWORK_COMMIT, expected $EXPECTED_FRAMEWORK_COMMIT"
for tag_pin in \
  framework-v3-rc5-20260828:a548d4b6982d70917ee79a0e584d45119c9dcbae \
  nxbootstrap-v0.6.36:5ab765d95d76d12fb425f5667f79d71c055118f2 \
  nxgenerator-v0.2.18:90953d8c5f565d5e380869776a4a5d9e44fdfa7a \
  nxrelease-v0.2.39:a548d4b6982d70917ee79a0e584d45119c9dcbae; do
  tag=${tag_pin%%:*}
  expected=${tag_pin#*:}
  actual=$(git -C "$FRAMEWORK_REPO" rev-parse "$tag^{}" 2>/dev/null) ||
    fail "immutable framework tag is missing: $tag"
  [[ $actual == $expected ]] || fail "$tag resolves to $actual, expected $expected"
done

GENERATOR=$FRAMEWORK_ROOT/nxgenerator/nxgenerator.py
REFRESH=$FRAMEWORK_ROOT/nxrelease/nx-refresh-pins.py
RENDER=$FRAMEWORK_ROOT/nxrelease/nx-render-manifest.py
NXRELEASE=$FRAMEWORK_ROOT/nxrelease/nxrelease.py
FINALIZE_SETTINGS=$SCRIPT_DIR/finalize-owner-settings.py
for tool in "$GENERATOR" "$REFRESH" "$RENDER" "$NXRELEASE" \
  "$FINALIZE_SETTINGS"; do
  [[ -f $tool ]] || fail "tool missing: $tool"
done

if [[ ${NX_SKIP_BUILD:-0} != 1 ]]; then
  (cd "$PORT_DIR" && ./build_universal.sh) || fail 'loader build failed'
fi
python3 -B "$REFRESH" --port-dir "$PORT_DIR" --framework-root "$FRAMEWORK_ROOT" ||
  fail 'runtime or authored payload pin refresh failed'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/$PORT_ID-package.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT INT TERM
GENERATOR_ROOT=$WORK/generated
MANIFEST=$GENERATOR_ROOT/nxrelease.json

python3 -B "$GENERATOR" "$PORT_DIR/nxproject.json" \
  --source-root "$PORT_DIR" --output "$GENERATOR_ROOT" ||
  fail 'nxgenerator failed'
python3 -B "$FINALIZE_SETTINGS" --port-dir "$PORT_DIR" \
  --generator-root "$GENERATOR_ROOT" --framework-root "$FRAMEWORK_ROOT" ||
  fail 'authored owner settings finalization failed'
python3 -B "$RENDER" --generator-root "$GENERATOR_ROOT" \
  --framework-root "$FRAMEWORK_ROOT" --source-url "$SOURCE_URL" \
  --source-date-epoch "$SOURCE_DATE_EPOCH" --max-glibc "$MAX_GLIBC" ||
  fail 'manifest render failed'

python3 -B "$NXRELEASE" validate --manifest "$MANIFEST" \
  --max-glibc "$MAX_GLIBC" || fail 'manifest validation failed'
python3 -B "$NXRELEASE" stage --manifest "$MANIFEST" \
  --stage "$WORK/preflight-stage" --max-glibc "$MAX_GLIBC" ||
  fail 'preflight stage failed'
python3 -B "$NXRELEASE" verify-stage --stage "$WORK/preflight-stage" \
  --max-glibc "$MAX_GLIBC" || fail 'preflight stage verification failed'

VERSION=$(python3 -B -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["package"]["version"])' \
  "$MANIFEST")
DESTINATION=${1:-"$PORT_DIR/.build/$PORT_ID-$VERSION-nxrelease"}
ARCHIVE_NAME=$PORT_ID.zip
[[ ! -e $DESTINATION ]] ||
  fail "destination already exists (never overwritten): $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

# This is the single archive-producing operation. All prior gates are ZIP-free.
python3 -B "$NXRELEASE" bundle --manifest "$MANIFEST" \
  --stage "$WORK/bundle-stage" --destination "$DESTINATION" \
  --archive-name "$ARCHIVE_NAME" --max-glibc "$MAX_GLIBC" ||
  fail 'bundle failed'

ARCHIVE=$DESTINATION/$ARCHIVE_NAME
[[ -f $ARCHIVE ]] || fail "archive not produced: $ARCHIVE"
python3 -B "$NXRELEASE" verify --archive "$ARCHIVE" \
  --max-glibc "$MAX_GLIBC" || fail 'archive verification failed'
python3 -B "$FRAMEWORK_ROOT/tests/audit-portmaster-zip.py" "$ARCHIVE" ||
  fail 'PortMaster ZIP audit failed'

sha256sum "$ARCHIVE" | awk -v name="$ARCHIVE_NAME" '{print $1"  "name}' \
  > "$ARCHIVE.sha256"
printf '%s PUBLIC PACKAGE PASS: %s\n' "$PORT_ID" "$ARCHIVE"
sha256sum "$ARCHIVE"
