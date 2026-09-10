#!/usr/bin/env bash
# Small redistributable checks: no IPA, guest code, display or target device.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf 'Usage: %s [idle rune services-events exit-chord ...]\nDefault: those four independent host fixtures. Requires Linux, C++17 and system SDL2 >= 2.0.14 for controller fixtures.\nHOST_CXX selects the host compiler; GOBLIN_HOST_TEST_BUILD selects the output directory.\n' "$0"
  exit 0
fi
if [[ $# == 0 ]]; then set -- idle rune services-events exit-chord; fi
needs_sdl=false
for fixture in "$@"; do
  case "$fixture" in
    idle|rune) ;;
    services-events|exit-chord) needs_sdl=true ;;
    *) printf 'Unknown fixture: %s; see --help.\n' "$fixture" >&2; exit 64 ;;
  esac
done
host_cxx=${HOST_CXX:-g++}
command -v -- "$host_cxx" >/dev/null || { printf 'Missing host C++ compiler: %s\n' "$host_cxx" >&2; exit 69; }
sdl_cflags=() sdl_libs=()
if "$needs_sdl"; then
  command -v pkg-config >/dev/null || { printf 'Controller fixtures require pkg-config and system SDL2 development files.\n' >&2; exit 69; }
  pkg-config --atleast-version=2.0.14 sdl2 || { printf 'System SDL2 >= 2.0.14 development files are required.\n' >&2; exit 69; }
  read -r -a sdl_cflags <<<"$(pkg-config --cflags sdl2)"
  read -r -a sdl_libs <<<"$(pkg-config --libs sdl2)"
fi
fixture_build=${GOBLIN_HOST_TEST_BUILD:-"$repo_root/build/host-tests"}
mkdir -p -- "$fixture_build"
# Negative subprocess cases intentionally abort; do not leave core dumps.
ulimit -c 0
printf 'Host checks: %s; compiler: %s; output: %s\n' "$*" "$host_cxx" "$fixture_build"
for fixture in "$@"; do
  printf 'BUILD %s\n' "$fixture"
  flags=(-std=c++17 -O2 -g -I"$repo_root/prototype")
  case "$fixture" in
    idle)
      "$host_cxx" "${flags[@]}" "$repo_root/tools/test_idle_host.cpp" \
        -Wl,--wrap=open,--wrap=read,--wrap=close,--wrap=write,--wrap=ioctl,--wrap=clock_gettime \
        -o "$fixture_build/$fixture" ;;
    rune)
      "$host_cxx" "${flags[@]}" "$repo_root/tools/test_darwin_rune.cpp" -o "$fixture_build/$fixture" ;;
    services-events)
      "$host_cxx" "${flags[@]}" "${sdl_cflags[@]}" "$repo_root/tools/host_services_event_fixture.cpp" \
        "$repo_root/prototype/services.cpp" "${sdl_libs[@]}" -o "$fixture_build/$fixture" ;;
    exit-chord)
      "$host_cxx" "${flags[@]}" "${sdl_cflags[@]}" "$repo_root/tools/host_exit_chord_fixture.cpp" \
        "${sdl_libs[@]}" -o "$fixture_build/$fixture" ;;
  esac
  printf 'RUN %s\n' "$fixture"
  "$fixture_build/$fixture"
  printf 'PASS %s\n' "$fixture"
done
