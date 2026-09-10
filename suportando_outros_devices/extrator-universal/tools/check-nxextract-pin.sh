#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Verify that a port vendors one complete, exact NXExtract release bundle.
# This copy is deliberately self-contained so standalone source archives can
# run their complete release gate without depending on the parent monorepo.
set -euo pipefail

export LC_ALL=C

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# A canonica e' o VERSION do proprio projeto -- um valor cravado aqui ja'
# apodreceu (ficou em 1.2.17) e recusava o bundle exatamente canonico.
CANONICAL_VERSION=$(tr -d '[:space:]' < "$PROJECT_ROOT/VERSION")
case "$CANONICAL_VERSION" in
  [0-9]*.[0-9]*.[0-9]*) : ;;
  *) printf 'NXEXTRACT PIN FAILED: VERSION is not a version: %s\n' \
       "$CANONICAL_VERSION" >&2; exit 1 ;;
esac
CANONICAL_ENGINE="$PROJECT_ROOT/nxextract.py"
CANONICAL_UI_MANIFEST="$PROJECT_ROOT/ui/release/manifest-v1.json"

fail() {
  printf 'NXEXTRACT PIN FAILED: %s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: %s --bundle PORT_DIR\n' "${0##*/}" >&2
  exit 2
}

read_version() {
  sed -n 's/^NXEXTRACT_VERSION = "\([^"]*\)".*/\1/p' "$1" | head -n 1
}

check_release_member() {
  local path=$1 links

  [ ! -L "$path" ] || fail "symbolic link is forbidden: $path"
  [ -f "$path" ] || fail "regular file is missing: $path"
  [ -s "$path" ] || fail "empty file is forbidden: $path"
  links=$(stat -c '%h' -- "$path" 2>/dev/null) ||
    fail "cannot inspect file: $path"
  [ "$links" = 1 ] || fail "hard-linked file is forbidden ($links links): $path"
}

same_file_hash() {
  local expected=$1 actual=$2 expected_hash actual_hash

  expected_hash=$(sha256sum -- "$expected" | awk '{print $1}')
  actual_hash=$(sha256sum -- "$actual" | awk '{print $1}')
  [ "$expected_hash" = "$actual_hash" ] ||
    fail "mixed NXExtract bundle: $actual differs from canonical $expected"
}

check_canonical_release() {
  local version engine_version

  check_release_member "$PROJECT_ROOT/VERSION"
  check_release_member "$CANONICAL_ENGINE"
  check_release_member "$PROJECT_ROOT/run-extractor.sh"
  check_release_member "$PROJECT_ROOT/nxextract-runtime-env.sh"
  check_release_member "$PROJECT_ROOT/docs/releases/$CANONICAL_VERSION.md"
  check_release_member "$CANONICAL_UI_MANIFEST"
  python3 -B "$PROJECT_ROOT/ui/tools/release-manifest.py" --verify >/dev/null ||
    fail "canonical multi-architecture UI release failed verification"
  version=$(tr -d '[:space:]' <"$PROJECT_ROOT/VERSION")
  engine_version=$(read_version "$CANONICAL_ENGINE")
  [ "$version" = "$CANONICAL_VERSION" ] ||
    fail "VERSION is $version, expected $CANONICAL_VERSION"
  [ "$engine_version" = "$CANONICAL_VERSION" ] ||
    fail "canonical engine is $engine_version, expected $CANONICAL_VERSION"
}

check_bundle() {
  local port_dir=$1 bundle_dir engine_version path ui_hash ui_match

  check_canonical_release
  [ -d "$port_dir" ] || fail "port directory is missing: $port_dir"
  port_dir=$(cd -- "$port_dir" && pwd -P)
  bundle_dir="$port_dir/nxextract"
  [ -d "$bundle_dir" ] && [ ! -L "$bundle_dir" ] ||
    fail "fixed bundle directory is missing or linked: $bundle_dir"

  check_release_member "$port_dir/extractor.json"
  check_release_member "$bundle_dir/nxextract.py"
  check_release_member "$bundle_dir/run-extractor.sh"
  check_release_member "$bundle_dir/nxextract-runtime-env.sh"
  check_release_member "$bundle_dir/nxextract-ui"

  for path in nxextract.py run-extractor.sh nxextract-runtime-env.sh; do
    if [ -e "$port_dir/$path" ] || [ -L "$port_dir/$path" ]; then
      fail "duplicate legacy-layout member would mix bundles: $port_dir/$path"
    fi
  done

  engine_version=$(read_version "$bundle_dir/nxextract.py")
  [ "$engine_version" = "$CANONICAL_VERSION" ] ||
    fail "bundle engine is $engine_version, expected exact $CANONICAL_VERSION"
  same_file_hash "$CANONICAL_ENGINE" "$bundle_dir/nxextract.py"
  same_file_hash "$PROJECT_ROOT/run-extractor.sh" \
    "$bundle_dir/run-extractor.sh"
  same_file_hash "$PROJECT_ROOT/nxextract-runtime-env.sh" \
    "$bundle_dir/nxextract-runtime-env.sh"

  ui_hash=$(sha256sum -- "$bundle_dir/nxextract-ui" | awk '{print $1}')
  ui_match=$(python3 -B - "$CANONICAL_UI_MANIFEST" "$ui_hash" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    manifest = json.load(stream)
matches = [
    architecture
    for architecture, record in manifest["artifacts"].items()
    if record["sha256"] == sys.argv[2]
]
if len(matches) != 1:
    raise SystemExit(1)
print(matches[0])
PY
  ) || fail "bundle UI does not match the immutable canonical UI release"

  printf 'NXEXTRACT BUNDLE OK: %s version=%s ui_architecture=%s\n' \
    "$port_dir" "$CANONICAL_VERSION" "$ui_match"
}

[ "$#" -eq 2 ] && [ "$1" = --bundle ] || usage
check_bundle "$2"
