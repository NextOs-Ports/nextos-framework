#!/usr/bin/env bash
# Hermetic release gate. It audits/builds but never executes owner game code.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
REPO_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
cd "$PORT_DIR"

HOST_TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gunbrick-host.XXXXXX")
cleanup_host_test() {
  find "$HOST_TEST_DIR" -type f -delete 2>/dev/null || true
  rmdir "$HOST_TEST_DIR" 2>/dev/null || true
}
trap cleanup_host_test EXIT INT TERM

${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
  -Isrc -Ivendor/nxloader/include tests/test_pthread_bridge.c \
  -o "$HOST_TEST_DIR/test-pthread-bridge"
"$HOST_TEST_DIR/test-pthread-bridge"

${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
  -Isrc tests/test_jni_refs.c src/jni_refs.c \
  -o "$HOST_TEST_DIR/test-jni-refs"
"$HOST_TEST_DIR/test-jni-refs"

for script in build.sh build_universal.sh "Gunbrick.sh" \
              nxextract/run-extractor.sh nxextract/nxextract-runtime-env.sh \
              package/build-package.sh; do
  bash -n "$script"
done

python3 -B nxextract/nxextract.py recipe-check --recipe extractor.json
./build_universal.sh
python3 -B tests/test_gunbrick_contract.py
if [ -n "${NXRELEASE:-}" ]; then
  python3 -B "$NXRELEASE" validate --manifest nxrelease.json
else
  echo "NXRELEASE not set; skipping manifest validation (internal NextOS release tool)"
fi

DRY_ADD=$(git -C "$REPO_ROOT" add -n --all ports/gunbrick)
if grep -E \
  "ports/gunbrick/(\.build|build|gamedata|stage|verify)/|ports/gunbrick/(gunbrick|gunbrick-nextos)'|\.(apk|apkm|apks|xapk|obb|so|zip|raw|png)'" \
  <<< "$DRY_ADD"; then
  printf '%s\n' 'Git dry-run would stage built or owner data' >&2
  exit 1
fi

git -C "$REPO_ROOT" diff --check -- ports/gunbrick
printf '%s\n' \
  'GUNBRICK HOST GATE: PASS' \
  'physical_device_evidence=0 baseline_physical_release=0.2.1 proprietary_payload_packaged=0 guest_execution=0'
