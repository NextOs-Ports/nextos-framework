#!/bin/bash
# PartyBoard — NextOS package builder (Mali-450/GLES2).
# Assembles a clean staging tree from the build output + the SDL3 mali driver.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD=""
SDL3=""
STAGE=""
ROM=""
MENU_IMAGE=""
STRIP_TOOL=""
LIBPNG=""
LIBZ=""

usage() {
  echo "Usage: $0 --build-dir DIR --sdl3 FILE --stage-dir DIR [--libpng FILE] [--libz FILE] [--rom FILE] [--menu-image FILE] [--strip-tool FILE]"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD="$2"; shift 2 ;;
    --sdl3) SDL3="$2"; shift 2 ;;
    --stage-dir) STAGE="$2"; shift 2 ;;
    --libpng) LIBPNG="$2"; shift 2 ;;
    --libz) LIBZ="$2"; shift 2 ;;
    --rom) ROM="$2"; shift 2 ;;
    --menu-image) MENU_IMAGE="$2"; shift 2 ;;
    --strip-tool) STRIP_TOOL="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[ -n "$BUILD" ] && [ -n "$SDL3" ] && [ -n "$STAGE" ] || { usage >&2; exit 2; }
[ -f "$BUILD/partyboard" ] || { echo "Missing binary: $BUILD/partyboard" >&2; exit 1; }
[ -f "$BUILD/libdol.so" ] || { echo "Missing libdol.so" >&2; exit 1; }
[ -f "$SDL3" ] || { echo "Missing SDL3 (mali): $SDL3" >&2; exit 1; }
[ ! -e "$STAGE" ] || { echo "Stage path already exists: $STAGE" >&2; exit 1; }
LIBPNG="${LIBPNG:-$BUILD/_deps/png-build/libpng16.so.16}"
LIBZ="${LIBZ:-$BUILD/_deps/zlib-build/libz.so.1}"
[ -f "$LIBPNG" ] || { echo "Missing libpng16: $LIBPNG" >&2; exit 1; }
[ -f "$LIBZ" ] || { echo "Missing libz: $LIBZ" >&2; exit 1; }
if [ -n "$STRIP_TOOL" ]; then
  [ -x "$STRIP_TOOL" ] || { echo "Invalid strip tool: $STRIP_TOOL" >&2; exit 1; }
elif [ -f "$BUILD/CMakeCache.txt" ]; then
  STRIP_TOOL="$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$BUILD/CMakeCache.txt" | head -1)"
fi
[ -n "$STRIP_TOOL" ] && [ -x "$STRIP_TOOL" ] || {
  echo "The official target strip tool was not found; pass --strip-tool explicitly" >&2
  exit 1
}

mkdir -p "$STAGE/ports/partyboard" "$STAGE/ports_scripts/images"
PORT="$STAGE/ports/partyboard"
mkdir -p "$PORT/assets" "$PORT/runtime/home" "$PORT/runtime/config" "$PORT/runtime/cache"

# Binary + RELs + shared libs (RUNPATH=$ORIGIN finds them beside the binary).
cp "$BUILD/partyboard" "$PORT/partyboard"
# All GameCube REL modules + the shared DOL (every *.so the build produced).
for so in "$BUILD"/*.so; do cp "$so" "$PORT/"; done

# SDL3 (mali-fbdev driver) goes in its own directory: the launcher prefers the
# firmware's SDL3 when it can satisfy our symbols, and reaches this copy only by
# putting this directory on the search path. Keeping it out of $PORT is what
# makes that choice possible at all.
mkdir -p "$PORT/sdl3-fallback"
cp "$SDL3" "$PORT/sdl3-fallback/libSDL3.so.0"
cp "$LIBPNG" "$PORT/libpng16.so.16"
cp "$LIBZ" "$PORT/libz.so.1"

# Resources + launcher + controller db + metadata.
cp -a "$ROOT/res" "$PORT/res"
cp "$ROOT/packaging/nextos/PartyBoard.sh" "$PORT/PartyBoard.sh"
cp "$ROOT/packaging/nextos/PartyBoard.sh" "$STAGE/ports_scripts/PartyBoard.sh"
cp "$ROOT/packaging/nextos/gamecontrollerdb.txt" "$PORT/gamecontrollerdb.txt"
cp "$ROOT/packaging/nextos/port.json" "$PORT/port.json"
cp "$ROOT/packaging/nextos/gameinfo.xml" "$PORT/gameinfo.xml"
# Learned on the complete board/minigame test flow. The pipeline seed moves
# shader creation to boot; the binary seed skips it entirely on the matching
# Mali-450 driver and self-invalidates when the GL fingerprint changes.
cp "$ROOT/packaging/nextos/initial_pipeline_cache.db" "$PORT/initial_pipeline_cache.db"
cp "$ROOT/packaging/nextos/initial_program_binary_cache.db" "$PORT/initial_program_binary_cache.db"

# Player-facing notes. These live in the repo so a rebuild reproduces the
# release instead of quietly dropping them.
cp "$ROOT/packaging/nextos/README.txt" "$STAGE/README.txt"
cp "$ROOT/packaging/nextos/PUT-YOUR-DISC-IMAGE-HERE.txt" "$PORT/assets/PUT-YOUR-DISC-IMAGE-HERE.txt"

mkdir -p "$PORT/licenses"
cp "$ROOT/packaging/nextos/NEXTOS-LICENSE" "$PORT/licenses/NEXTOS-LICENSE"
cp "$ROOT/NOTICE.md" "$PORT/licenses/NOTICE.md"
cp "$ROOT/README.md" "$PORT/licenses/PARTYBOARD-README.md" 2>/dev/null || true
cp "$ROOT/extern/aurora/LICENSE" "$PORT/licenses/AURORA-LICENSE" 2>/dev/null || true
cp "$ROOT/extern/libco/LICENSE" "$PORT/licenses/LIBCO-LICENSE" 2>/dev/null || true
cp "$ROOT/extern/musyx/LICENSE" "$PORT/licenses/MUSYX-LICENSE" 2>/dev/null || true

# Optional disc image (private; not in public/source distributions).
if [ -n "$ROM" ]; then
  cp "$ROM" "$PORT/assets/$(basename "$ROM")"
fi

# Menu art.
if [ -n "$MENU_IMAGE" ]; then
  cp "$MENU_IMAGE" "$STAGE/ports_scripts/images/PartyBoard.png"
elif [ -f "$ROOT/packaging/nextos/PartyBoard-menu.png" ]; then
  cp "$ROOT/packaging/nextos/PartyBoard-menu.png" "$STAGE/ports_scripts/images/PartyBoard.png"
elif [ -f "$PORT/res/logo.png" ]; then
  cp "$PORT/res/logo.png" "$STAGE/ports_scripts/images/PartyBoard.png"
fi

# Strip every recompilable target binary with the same official NextOS
# toolchain used by the build, then drop build-specific RUNPATH entries
# (launcher sets LD_LIBRARY_PATH).
"$STRIP_TOOL" --strip-unneeded "$PORT/partyboard" "$PORT"/*.so "$PORT"/lib*.so.* \
  "$PORT/sdl3-fallback/libSDL3.so.0"
if command -v patchelf >/dev/null 2>&1; then
  for elf in "$PORT/partyboard" "$PORT"/*.so "$PORT"/lib*.so.* \
             "$PORT/sdl3-fallback/libSDL3.so.0"; do
    patchelf --remove-rpath "$elf"
  done
fi

find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$PORT/partyboard" "$PORT/PartyBoard.sh" "$STAGE/ports_scripts/PartyBoard.sh"

echo "Staged clean package at: $STAGE"
du -sh "$STAGE"
echo "RELs (m4xx/w0x): $(ls "$PORT"/m*.so "$PORT"/w*Dll.so 2>/dev/null | wc -l)"
