#!/usr/bin/env bash
# Hermetic release gate. It audits but never executes owner game code.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export PYTHONDONTWRITEBYTECODE=1

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$PORT_DIR"

for script in build_universal.sh "Star Wars KOTOR.sh" port-env.sh \
              nxextract/run-extractor.sh nxextract/nxextract-runtime-env.sh \
              package/build-package.sh; do
  bash -n "$script"
done

python3 -B nxextract/nxextract.py recipe-check --recipe extractor.json
python3 -B tests/test_kotor_contract.py
bash tests/test-language.sh
if [ -n "${NXRELEASE:-}" ]; then
  python3 -B "$NXRELEASE" validate --manifest nxrelease.json
else
  echo "NXRELEASE not set; skipping manifest validation (internal NextOS release tool)"
fi

DRY_ADD=$(git -C "$PORT_DIR" add -n --all .)
if grep -E \
  "(dist|gamedata|verify|stage)/|\.(apk|apkm|apks|xapk|obb|zip|raw|png)'" \
  <<< "$DRY_ADD"; then
  printf '%s\n' 'Git dry-run would stage built or owner data' >&2
  exit 1
fi

git -C "$PORT_DIR" diff --check
printf '%s\n' \
  'KOTOR HOST GATE: PASS' \
  'physical_device_evidence=1 proprietary_payload_packaged=0 guest_execution=0'
