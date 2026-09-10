#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
port_dir=$(CDPATH= cd -- "$test_dir/.." && pwd -P)
cc_path=$(command -v cc) || {
  printf 'huntdown display contract failed: missing host C compiler\n' >&2
  exit 1
}
stage=$(mktemp -d "${TMPDIR:-/tmp}/huntdown-display-contract.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT INT TERM

"$cc_path" -std=c11 -Wall -Wextra -Werror \
  -I "$port_dir/src" \
  "$test_dir/test_display_contract.c" \
  "$port_dir/src/huntdown_display.c" \
  -o "$stage/test-display-contract"
"$stage/test-display-contract"
