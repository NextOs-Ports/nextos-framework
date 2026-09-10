#!/usr/bin/env bash
# Host-side wrapper: build the PortMaster / SDL2-shim variant in the same focal
# container the Dusklight PortMaster package is built with.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_TREE="${PARTYBOARD_SDL2SHIM_SRC:-$ROOT/../SDL3-sdl2shim-current}"
IMAGE="${PARTYBOARD_PM_IMAGE:-dusklight-aarch64-focal-sdl2shim:alpha13}"

[ -d "$SDL_TREE/src/video/sdl2" ] || {
  echo "Patched SDL2-shim tree not found: $SDL_TREE" >&2
  echo "Set PARTYBOARD_SDL2SHIM_SRC, or rebuild it from packaging/portmaster/patches." >&2
  exit 1
}

exec docker run --rm \
  -v "$ROOT":/work \
  -v "$SDL_TREE":/sdl \
  -w /work \
  "$IMAGE" \
  bash -lc "/work/packaging/portmaster/configure-portmaster.sh"
