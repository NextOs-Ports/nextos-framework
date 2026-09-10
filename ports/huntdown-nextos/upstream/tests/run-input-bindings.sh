#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
port_dir=$(CDPATH= cd -- "$test_dir/.." && pwd -P)
cc_path=$(command -v cc) || {
  printf 'huntdown input contract failed: missing host C compiler\n' >&2
  exit 1
}
stage=$(mktemp -d "${TMPDIR:-/tmp}/huntdown-input-contract.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT INT TERM

"$cc_path" -std=c11 -Wall -Wextra -Werror \
  -I "$port_dir/src" "$test_dir/test_input_bindings.c" \
  -o "$stage/test-input-bindings"
"$stage/test-input-bindings"
