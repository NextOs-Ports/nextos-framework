#!/usr/bin/env bash
# Build, gate and bundle the public Retro City Rampage DX BYO-data release.
#
# The port already had every gate (test-final-zip.sh) and every hash
# (refresh-release-manifest.py); what it lacked was the one step that turns
# them into an archive, so a release was assembled by hand each time and the
# framework's one-command path could not reach it. This is that step, in the
# same shape as the other ports' package scripts.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$PORT_DIR/../.." && pwd -P)
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:-${NX_FRAMEWORK_ROOT:-$REPO_ROOT/framework}}
NXRELEASE="$FRAMEWORK_ROOT/nxrelease/nxrelease.py"
MANIFEST="$PORT_DIR/nxrelease.json"
VERSION=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["package"]["version"])' "$MANIFEST")
DESTINATION=${1:-"$PORT_DIR/.build/rcrdx-$VERSION-nxrelease"}
ARCHIVE_NAME="rcrdx.zip"

fail() { printf 'rcrdx package error: %s\n' "$*" >&2; exit 1; }

[[ -f $NXRELEASE ]] || fail "release tool missing: $NXRELEASE"
[[ -x $PORT_DIR/build_universal.sh ]] || fail 'build_universal.sh missing'

# The loader is always rebuilt from source: a package that ships a binary
# nobody rebuilt is how a fix in src/ silently never reaches a device.
if [[ ${RCRDX_SKIP_BUILD:-0} != 1 ]]; then
  (cd "$PORT_DIR" && ./build_universal.sh) || fail 'loader build failed'
fi
[[ -f $PORT_DIR/rcrdx-nextos ]] || fail 'rcrdx-nextos not produced'

# Hashes come from the files, through the port's own reviewed allowlist.
python3 -B "$SCRIPT_DIR/refresh-release-manifest.py" || fail 'manifest refresh failed'

python3 -B "$FRAMEWORK_ROOT/nxabi/nxabi.py" audit \
  --sdl-floor 2.0.4 "$PORT_DIR/rcrdx-nextos" || fail 'ABI audit failed'
python3 -B "$NXRELEASE" validate --manifest "$MANIFEST" --max-glibc 2.27 ||
  fail 'manifest validation failed'

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/rcrdx-package.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT INT TERM

[[ ! -e $DESTINATION ]] || fail "destination already exists (release outputs are never overwritten): $DESTINATION"
mkdir -p -- "$(dirname -- "$DESTINATION")"

python3 -B "$NXRELEASE" bundle \
  --manifest "$MANIFEST" --stage "$STAGE/stage" \
  --destination "$DESTINATION" --archive-name "$ARCHIVE_NAME" \
  --max-glibc 2.27 || fail 'bundle failed'

ARCHIVE="$DESTINATION/$ARCHIVE_NAME"
[[ -f $ARCHIVE ]] || fail "archive not produced: $ARCHIVE"

# The same gates the port always ran on a finished zip, now on the one it just
# produced rather than one someone assembled elsewhere.
"$SCRIPT_DIR/test-final-zip.sh" "$ARCHIVE" || fail 'final zip gate failed'

sha256sum "$ARCHIVE" | awk '{print $1"  '"$ARCHIVE_NAME"'"}' > "$ARCHIVE.sha256"
printf 'RCRDX PUBLIC PACKAGE PASS: %s\n' "$ARCHIVE"
sha256sum "$ARCHIVE"
