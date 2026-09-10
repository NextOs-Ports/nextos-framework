#!/usr/bin/env bash
set -euo pipefail
study_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf 'Usage: %s --macho PATH\nReads owner-supplied ARM64 metadata; never executes guest code.\n' "$0"; exit 0
fi
if [[ $# != 2 || $1 != --macho || ! -f $2 ]]; then
  printf 'Expected --macho PATH to an existing owner-supplied Mach-O.\n' >&2; exit 64
fi
host_cxx=${HOST_CXX:-g++}
command -v -- "$host_cxx" >/dev/null || { printf 'Missing host C++ compiler: %s\n' "$host_cxx" >&2; exit 69; }
fixture_build=${GOBLIN_HOST_TEST_BUILD:-"$study_root/build/host-tests"}
mkdir -p -- "$fixture_build"
printf 'Directed protocol fixture; output: %s\n' "$fixture_build" >&2
"$host_cxx" -std=c++17 -O2 -I"$study_root/prototype" \
  "$study_root/tools/host_protocol_fixture.cpp" "$study_root/prototype/macho_loader.cpp" \
  "$study_root/prototype/darwin_libc.cpp" -ldl -pthread -o "$fixture_build/protocol-test"
"$fixture_build/protocol-test" "$2"
