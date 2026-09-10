#!/bin/bash
set -euo pipefail

PORT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$PORT_ROOT/../.." && pwd -P)
# The framework under test is the pinned one, not whatever the monorepo checkout
# happens to carry; a release built against 0.6.16 must be gated by 0.6.16.
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:-${NX_FRAMEWORK_ROOT:-$REPO_ROOT/framework}}
ARCHIVE=${1:?usage: test-final-zip.sh ARCHIVE [SHA256_FILE]}
SHA_FILE=${2:-}

"$PORT_ROOT/tests/test_video_provider.sh"
python3 -B "$FRAMEWORK_ROOT/nxabi/nxabi.py" audit \
  --sdl-floor 2.0.4 "$PORT_ROOT/rcrdx-nextos"
python3 -B "$FRAMEWORK_ROOT/nxrelease/nxrelease.py" validate \
  --manifest "$PORT_ROOT/nxrelease.json" --max-glibc 2.27

verify=(python3 -B "$FRAMEWORK_ROOT/nxrelease/nxrelease.py" verify
        --archive "$ARCHIVE" --max-glibc 2.27)
if [[ -n "$SHA_FILE" ]]; then
  verify+=(--sha256-file "$SHA_FILE")
fi
"${verify[@]}"

python3 -B "$FRAMEWORK_ROOT/tests/audit-portmaster-zip.py" "$ARCHIVE"
python3 -B "$PORT_ROOT/package/check-final-zip.py" "$ARCHIVE"
echo "RCRDX final ZIP gate: PASS"
