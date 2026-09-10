#!/bin/bash
# PartyBoard — PortMaster (SDL2-shim) package builder.
# Assembles the zip-ready tree: launcher at the top, everything else under
# partyboard/. Ships no game data.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build/portmaster-aarch64-focal-sdl2shim}"
STAGE=""
ARCH="aarch64"
STRIP_TOOL="${STRIP_TOOL:-}"

usage() { echo "Usage: $0 --stage-dir DIR [--build-dir DIR] [--strip-tool FILE]"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD="$2"; shift 2 ;;
    --stage-dir) STAGE="$2"; shift 2 ;;
    --strip-tool) STRIP_TOOL="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[ -n "$STAGE" ] || { usage >&2; exit 2; }
[ -f "$BUILD/partyboard" ] || { echo "Missing binary: $BUILD/partyboard" >&2; exit 1; }
[ -f "$BUILD/libdol.so" ] || { echo "Missing libdol.so" >&2; exit 1; }
[ ! -e "$STAGE" ] || { echo "Stage path already exists: $STAGE" >&2; exit 1; }

SDL3="$(ls "$BUILD"/_deps/sdl-build/libSDL3.so.0.* 2>/dev/null | head -1)"
[ -f "$SDL3" ] || { echo "Missing built SDL3 (sdl2-shim) under $BUILD/_deps/sdl-build" >&2; exit 1; }

if [ -z "$STRIP_TOOL" ] && [ -f "$BUILD/CMakeCache.txt" ]; then
  STRIP_TOOL="$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$BUILD/CMakeCache.txt" | head -1)"
fi

PORT="$STAGE/partyboard"
mkdir -p "$PORT/lib.$ARCH" "$PORT/assets" "$PORT/runtime/home" "$PORT/runtime/config" "$PORT/runtime/cache" "$PORT/licenses"

cp "$BUILD/partyboard" "$PORT/partyboard.$ARCH"
# libdol.so + the 92 GameCube REL modules. objdll.c dlopen()s them by bare name,
# so they only need to sit on LD_LIBRARY_PATH, which the launcher points here.
for so in "$BUILD"/*.so; do cp "$so" "$PORT/lib.$ARCH/"; done
cp "$SDL3" "$PORT/lib.$ARCH/libSDL3.so.0"

cp -a "$ROOT/res" "$PORT/res"
cp "$ROOT/packaging/portmaster/aarch64/PartyBoard.sh" "$STAGE/PartyBoard.sh"
cp "$ROOT/packaging/portmaster/aarch64/partyboard/partyboard.gptk" "$PORT/partyboard.gptk"
cp "$ROOT/packaging/portmaster/aarch64/partyboard/port.json" "$PORT/port.json"
cp "$ROOT/packaging/nextos/gamecontrollerdb.txt" "$PORT/gamecontrollerdb.txt"

cp "$ROOT/README.md" "$PORT/licenses/PARTYBOARD-README.md" 2>/dev/null || true
cp "$ROOT/NOTICE.md" "$PORT/licenses/PARTYBOARD-NOTICE.md" 2>/dev/null || true
cp "$ROOT/extern/aurora/LICENSE" "$PORT/licenses/AURORA-LICENSE" 2>/dev/null || true
cp "$ROOT/extern/libco/LICENSE" "$PORT/licenses/LIBCO-LICENSE" 2>/dev/null || true
cp "$ROOT/extern/musyx/LICENSE" "$PORT/licenses/MUSYX-LICENSE" 2>/dev/null || true

if [ -n "$STRIP_TOOL" ] && [ -x "$STRIP_TOOL" ]; then
  "$STRIP_TOOL" --strip-unneeded "$PORT/partyboard.$ARCH" "$PORT/lib.$ARCH"/*.so "$PORT/lib.$ARCH"/libSDL3.so.0
elif command -v "${ARCH}-linux-gnu-strip" >/dev/null 2>&1; then
  "${ARCH}-linux-gnu-strip" --strip-unneeded "$PORT/partyboard.$ARCH" "$PORT/lib.$ARCH"/*.so
else
  echo "warning: no target strip tool found; shipping unstripped binaries" >&2
fi

find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/PartyBoard.sh" "$PORT/partyboard.$ARCH"

echo "Staged PortMaster package at: $STAGE"
du -sh "$STAGE"
echo "RELs: $(ls "$PORT/lib.$ARCH"/*Dll.so 2>/dev/null | wc -l)"
