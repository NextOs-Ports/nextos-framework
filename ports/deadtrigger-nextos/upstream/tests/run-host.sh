#!/usr/bin/env bash
# Hermetic release gate; never executes owner Android code.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
REPO_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
cd "$PORT_DIR"

for script in build.sh build_universal.sh "Dead Trigger.sh" \
              nxextract/run-extractor.sh nxextract/nxextract-runtime-env.sh \
              package/build-package.sh; do
  bash -n "$script"
done
python3 -B nxextract/nxextract.py recipe-check --recipe extractor.json

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/deadtrigger-gate.XXXXXX")
cleanup() {
  case $WORK_ROOT in
    "${TMPDIR:-/tmp}"/deadtrigger-gate.*)
      [[ -d $WORK_ROOT ]] && rm -rf -- "$WORK_ROOT"
      ;;
    *) printf 'refusing unsafe cleanup target: %s\n' "$WORK_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT INT TERM

./build_universal.sh
cp -- build/deadtrigger-nextos "$WORK_ROOT/first"
./build_universal.sh
cmp -- "$WORK_ROOT/first" build/deadtrigger-nextos

python3 -B tests/test_deadtrigger_contract.py
python3 -B ../../framework/nxrelease/nxrelease.py validate \
  --manifest nxrelease.json

DRY_ADD=$(git -C "$REPO_ROOT" add -n --all ports/deadtrigger)
if grep -E \
  "ports/deadtrigger/(build|game|payload|dump_out)/|ports/deadtrigger/deadtrigger'|\.(apk|apkm|apks|xapk|obb|so|zip|core|log)'" \
  <<< "$DRY_ADD"; then
  printf '%s\n' 'Git dry-run would stage built or owner data' >&2
  exit 1
fi

git -C "$REPO_ROOT" diff --check -- ports/deadtrigger
printf '%s\n' \
  'DEAD TRIGGER HOST GATE: PASS' \
  'reproducible=1 physical_device_evidence=1 proprietary_payload_packaged=0 guest_execution=0'
