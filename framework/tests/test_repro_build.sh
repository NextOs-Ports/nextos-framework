#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3-REPRO-01 gate for framework/tools/nx-repro-build.sh.
#
# 1. positive: nx-repro-build.sh (without --with-elf) exits 0 and writes a
#    BUILD-PROVENANCE.json that is strict JSON, carries the schema fields,
#    verdict "identical", and leaks no /home/ literal and no IPv4;
# 2. stability: a SECOND run produces byte-wise identical output sha
#    manifests inside the provenance (build-of-build stability);
# 3. negative: with NXREPRO_TEST_MUTATE=1 the tool must FAIL and name the
#    mutated file.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
TOOL="$REPO_ROOT/framework/tools/nx-repro-build.sh"

WORK_PARENT=${TMPDIR:-/tmp}
WORK_PARENT=$(cd -- "$WORK_PARENT" && pwd -P)
WORK=$(mktemp -d "$WORK_PARENT/test-repro-build.XXXXXX")
cleanup() {
  local status=$?
  trap - EXIT
  case $WORK in
    "$WORK_PARENT"/test-repro-build.??????) rm -rf -- "$WORK" ;;
    *) printf 'test_repro_build: refused cleanup outside owned mktemp: %s\n' \
         "$WORK" >&2; status=1 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() { printf 'test_repro_build: FAIL %s\n' "$*" >&2; exit 1; }
step() { printf '[test_repro_build] %s\n' "$*"; }

[[ -x $TOOL ]] || fail "tool missing or not executable: $TOOL"

# --- 1. positive run ---------------------------------------------------------
OUT1="$WORK/out1"
LOG1="$WORK/run1.log"
step "positive run 1 -> $OUT1"
if ! "$TOOL" --out "$OUT1" > "$LOG1" 2>&1; then
  tail -n 40 -- "$LOG1" >&2
  fail "nx-repro-build.sh exited non-zero on the positive run"
fi
PROV1="$OUT1/BUILD-PROVENANCE.json"
[[ -f $PROV1 ]] || fail "BUILD-PROVENANCE.json not written: $PROV1"

step "validating provenance schema + verdict (strict JSON)"
python3 - "$PROV1" <<'PYEOF' || fail "provenance validation failed"
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    doc = json.load(handle)  # strict JSON or die
assert doc["schema"] == "nx-build-provenance-v1", doc.get("schema")
assert doc["schema_version"] == 1, doc.get("schema_version")
for field in ("head_commit", "environment", "tools", "generator",
              "outputs", "verdict", "commands"):
    assert field in doc, "missing field: %s" % field
assert doc["verdict"] == "identical", doc["verdict"]
assert len(doc["head_commit"]) == 40
assert doc["environment"]["source_date_epoch"] == 1786492800
assert doc["outputs"]["tree_a"], "tree_a manifest empty"
assert doc["outputs"]["tree_a"] == doc["outputs"]["tree_b"]
assert doc["generator"]["version"], "generator version missing"
PYEOF

step "scanning provenance for /home/ literals and IPv4 addresses"
if grep -q '/home/' -- "$PROV1"; then
  fail "provenance contains a /home/ literal"
fi
if grep -Eq '(^|[^0-9.])([0-9]{1,3}\.){3}[0-9]{1,3}([^0-9.]|$)' -- "$PROV1"; then
  fail "provenance contains an IPv4-looking literal"
fi

# --- 2. build-of-build stability --------------------------------------------
OUT2="$WORK/out2"
LOG2="$WORK/run2.log"
step "positive run 2 (stability) -> $OUT2"
if ! "$TOOL" --out "$OUT2" > "$LOG2" 2>&1; then
  tail -n 40 -- "$LOG2" >&2
  fail "second nx-repro-build.sh run exited non-zero"
fi
PROV2="$OUT2/BUILD-PROVENANCE.json"
[[ -f $PROV2 ]] || fail "second BUILD-PROVENANCE.json not written"

step "comparing the two provenance output manifests"
python3 - "$PROV1" "$PROV2" <<'PYEOF' || fail "provenance manifests differ between run 1 and run 2"
import json, sys
docs = []
for path in sys.argv[1:3]:
    with open(path, "r", encoding="utf-8") as handle:
        docs.append(json.load(handle))
assert docs[0]["outputs"] == docs[1]["outputs"], \
    "sha manifests differ across independent runs"
assert docs[0]["head_commit"] == docs[1]["head_commit"]
PYEOF

# --- 3. negative: injected mutation must fail naming the file ---------------
OUT3="$WORK/out3"
LOG3="$WORK/run3.log"
step "negative run with NXREPRO_TEST_MUTATE=1"
set +e
NXREPRO_TEST_MUTATE=1 "$TOOL" --out "$OUT3" > "$LOG3" 2>&1
NEG_STATUS=$?
set -e
if (( NEG_STATUS == 0 )); then
  tail -n 40 -- "$LOG3" >&2
  fail "mutated run unexpectedly PASSED"
fi
MUTATED=$(sed -n 's/^.*appending one byte to tree B file: *//p' "$LOG3" | head -n1)
[[ -n $MUTATED ]] || { tail -n 40 -- "$LOG3" >&2
  fail "mutate hook did not announce a target file"; }
if ! grep -F -q -- "$MUTATED" <(grep -A100 'DIVERGENT paths' "$LOG3"); then
  tail -n 40 -- "$LOG3" >&2
  fail "failure output does not name the mutated file: $MUTATED"
fi
if [[ -f $OUT3/BUILD-PROVENANCE.json ]]; then
  fail "provenance must NOT be written on a failed comparison"
fi
step "negative run failed as required naming: $MUTATED"

printf 'repro build gate passed: worktrees=2 identical=1 provenance=1 negative=1\n'
