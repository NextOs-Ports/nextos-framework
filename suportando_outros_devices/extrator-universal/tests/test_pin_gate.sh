#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
PIN_GATE="$PROJECT_ROOT/tools/check-nxextract-pin.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxextract-pin-gate.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

fail() {
  printf 'NXExtract pin regression failed: %s\n' "$*" >&2
  exit 1
}

PORT_DIR="$TEST_ROOT/port sample"
BUNDLE_DIR="$PORT_DIR/nxextract"
mkdir -p "$BUNDLE_DIR"
cp "$PROJECT_ROOT/examples/recipe-minimal.json" "$PORT_DIR/extractor.json"
cp "$PROJECT_ROOT/nxextract.py" "$BUNDLE_DIR/nxextract.py"
cp "$PROJECT_ROOT/run-extractor.sh" "$BUNDLE_DIR/run-extractor.sh"
cp "$PROJECT_ROOT/nxextract-runtime-env.sh" \
  "$BUNDLE_DIR/nxextract-runtime-env.sh"
cp "$PROJECT_ROOT/ui/release/aarch64/nxextract-ui" "$BUNDLE_DIR/nxextract-ui"

mv "$PORT_DIR/extractor.json" "$TEST_ROOT/recipe-real"
ln "$TEST_ROOT/recipe-real" "$PORT_DIR/extractor.json"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "a hard-linked recipe was accepted"
fi
rm "$PORT_DIR/extractor.json"
cp "$PROJECT_ROOT/examples/recipe-minimal.json" "$PORT_DIR/extractor.json"

: >"$BUNDLE_DIR/nxextract.py"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "an empty engine was accepted"
fi
cp "$PROJECT_ROOT/nxextract.py" "$BUNDLE_DIR/nxextract.py"

"$PIN_GATE" --bundle "$PORT_DIR" >/dev/null ||
  fail "an exact canonical bundle was rejected"

printf '\n# mixed runner\n' >>"$BUNDLE_DIR/run-extractor.sh"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "a mixed-version runner was accepted"
fi
cp "$PROJECT_ROOT/run-extractor.sh" "$BUNDLE_DIR/run-extractor.sh"

mv "$BUNDLE_DIR/nxextract-runtime-env.sh" "$TEST_ROOT/runtime-real"
ln -s "$TEST_ROOT/runtime-real" "$BUNDLE_DIR/nxextract-runtime-env.sh"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "a symlinked runtime helper was accepted"
fi
rm "$BUNDLE_DIR/nxextract-runtime-env.sh"
cp "$PROJECT_ROOT/nxextract-runtime-env.sh" \
  "$BUNDLE_DIR/nxextract-runtime-env.sh"

cp "$PROJECT_ROOT/nxextract.py" "$PORT_DIR/nxextract.py"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "a duplicate legacy-layout engine was accepted"
fi
rm "$PORT_DIR/nxextract.py"

"$PIN_GATE" --bundle "$PORT_DIR" >/dev/null ||
  fail "the canonical mandatory UI pin was rejected"
printf 'mixed-ui' >>"$BUNDLE_DIR/nxextract-ui"
if "$PIN_GATE" --bundle "$PORT_DIR" >/dev/null 2>&1; then
  fail "a mixed UI binary was accepted"
fi

printf 'NXExtract pin regression tests passed\n'
