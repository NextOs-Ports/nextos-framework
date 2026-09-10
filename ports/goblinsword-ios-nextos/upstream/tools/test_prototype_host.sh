#!/usr/bin/env bash
set -euo pipefail

study_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_component=${1:-help}
case "$test_component" in
  runtime|services) ;;
  help|--help|-h)
    printf 'Usage: %s runtime --macho PATH | services\nRuntime requires the owner-supplied ARM64 Mach-O; services uses system SDL2 virtual input.\n' "$0"
    exit 0 ;;
  *) printf 'Unknown fixture: %s\n' "$test_component" >&2; exit 64 ;;
esac
shift
macho_path=
if [[ "$test_component" == runtime ]]; then
  if [[ $# != 2 || $1 != --macho || ! -f $2 ]]; then
    printf 'runtime requires --macho PATH to an existing owner-supplied Mach-O.\n' >&2; exit 64
  fi
  macho_path=$2
elif [[ $# != 0 ]]; then
  printf 'services accepts no arguments; see --help.\n' >&2; exit 64
fi
host_cxx=${HOST_CXX:-g++}
command -v -- "$host_cxx" >/dev/null || { printf 'Missing host C++ compiler: %s\n' "$host_cxx" >&2; exit 69; }

# These host diagnostics never replace the target executable.
fixture_build=${GOBLIN_HOST_TEST_BUILD:-"$study_root/build/host-tests"}
mkdir -p -- "$fixture_build"
printf 'Host fixture: %s; output: %s\n' "$test_component" "$fixture_build"
if [[ "$test_component" == runtime ]]; then
  "$host_cxx" -std=c++17 -O2 -I"$study_root/prototype" \
    "$study_root/tools/host_runtime_fixture.cpp" "$study_root/prototype/macho_loader.cpp" \
    "$study_root/prototype/darwin_libc.cpp" \
    -ldl -pthread -o "$fixture_build/runtime-test"
  "$fixture_build/runtime-test" "$macho_path"
else
  "$host_cxx" -std=c++17 -Wall -Wextra -Werror -O2 -I"$study_root/prototype" \
    "$study_root/tools/host_services_fixture.cpp" "$study_root/prototype/services.cpp" \
    -lSDL2 -o "$fixture_build/services-test"
  "$fixture_build/services-test"
fi
