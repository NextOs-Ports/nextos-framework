#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
port_dir=$(CDPATH= cd -- "$test_dir/.." && pwd -P)
cc_path=$(command -v cc) || {
  printf 'huntdown EGL contract failed: missing host C compiler\n' >&2
  exit 1
}
stage=$(mktemp -d "${TMPDIR:-/tmp}/huntdown-egl-contract.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT INT TERM

"$cc_path" -std=c11 -Wall -Wextra -Werror \
  -I "$port_dir/src" \
  "$test_dir/test_egl_config_contract.c" \
  "$port_dir/src/egl_config_contract.c" \
  -o "$stage/test-egl-config-contract"
"$stage/test-egl-config-contract"
