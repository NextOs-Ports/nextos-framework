#!/bin/bash
# Reproduce Godot's thirdparty/sdl/update-sdl.sh offline from the pinned SDL
# 3.2.30 source tree. Only the subset Godot intentionally vendors is copied.
set -euo pipefail
shopt -s nullglob

if [ "$#" -ne 2 ]; then
	printf 'usage: %s SDL_SOURCE GODOT_THIRDPARTY_SDL\n' "$0" >&2
	exit 2
fi

SDL_SOURCE=$1
TARGET=$2

[ -f "$SDL_SOURCE/src/SDL.c" ] && [ -f "$SDL_SOURCE/LICENSE.txt" ] || {
	printf 'invalid SDL source tree: %s\n' "$SDL_SOURCE" >&2
	exit 1
}
[ -f "$TARGET/update-sdl.sh" ] && [ "$(basename -- "$TARGET")" = sdl ] || {
	printf 'refusing unexpected Godot SDL target: %s\n' "$TARGET" >&2
	exit 1
}

for path in atomic core dynapi events haptic hidapi include io joystick libm loadso \
	sensor stdlib thread timer; do
	rm -rf -- "$TARGET/$path"
done
find "$TARGET" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \) -delete
rm -f -- "$TARGET/CREDITS.md" "$TARGET/LICENSE.txt"

install -m 0644 "$SDL_SOURCE/CREDITS.md" "$SDL_SOURCE/LICENSE.txt" "$TARGET/"
cp -a -- "$SDL_SOURCE/include" "$TARGET/include"
rm -f -- "$TARGET"/include/build_config/*.cmake \
	"$TARGET"/include/build_config/SDL_build_config_*.h

# Keep the complete public-header closure. SDL 3.2.30's umbrella SDL.h includes
# SDL_gpu.h even when the GPU subsystem is not compiled; deleting optional API
# headers, as Godot's older 3.2.28 updater did, therefore leaves an invalid
# vendored source tree. The source allowlist below still controls which SDL
# implementations enter the engine.

cp -a -- "$SDL_SOURCE"/src/*.{c,h} "$TARGET/"
cp -a -- "$SDL_SOURCE/src/atomic" "$SDL_SOURCE/src/libm" \
	"$SDL_SOURCE/src/stdlib" "$TARGET/"
cp -a -- "$SDL_SOURCE/src/dynapi" "$TARGET/dynapi"
rm -f -- "$TARGET"/stdlib/*.masm

# SDL 3.2.30 moved several disabled-subsystem declarations behind internal
# headers included by common translation units (SDL.c includes audio, camera,
# process and video headers even when those implementations are disabled).
# Preserve every internal header while still compiling only Godot's explicit
# source allowlist below.
while IFS= read -r -d '' header; do
	relative=${header#"$SDL_SOURCE/src/"}
	mkdir -p "$TARGET/$(dirname -- "$relative")"
	install -m 0644 "$header" "$TARGET/$relative"
done < <(find "$SDL_SOURCE/src" -type f -name '*.h' -print0)

mkdir -p "$TARGET/events" "$TARGET/io" "$TARGET/core" "$TARGET/haptic" \
	"$TARGET/joystick" "$TARGET/loadso" "$TARGET/sensor" "$TARGET/thread" \
	"$TARGET/timer" "$TARGET/hidapi"
cp -a -- "$SDL_SOURCE"/src/events/SDL_event*.{c,h} \
	"$SDL_SOURCE/src/events/SDL_mouse_c.h" "$TARGET/events/"
cp -a -- "$SDL_SOURCE"/src/io/SDL_iostream*.{c,h} "$TARGET/io/"
cp -a -- "$SDL_SOURCE/src/core/linux" "$SDL_SOURCE/src/core/unix" \
	"$SDL_SOURCE/src/core/windows" "$TARGET/core/"
rm -f -- "$TARGET/core/windows/version.rc" \
	"$TARGET"/core/linux/SDL_fcitx.* "$TARGET"/core/linux/SDL_ibus.* \
	"$TARGET"/core/linux/SDL_ime.* "$TARGET"/core/linux/SDL_system_theme.*
cp -a -- "$SDL_SOURCE"/src/haptic/*.{c,h} "$SDL_SOURCE/src/haptic/darwin" \
	"$SDL_SOURCE/src/haptic/linux" "$SDL_SOURCE/src/haptic/windows" "$TARGET/haptic/"
cp -a -- "$SDL_SOURCE"/src/joystick/*.{c,h} \
	"$SDL_SOURCE/src/joystick/apple" "$SDL_SOURCE/src/joystick/darwin" \
	"$SDL_SOURCE/src/joystick/hidapi" "$SDL_SOURCE/src/joystick/linux" \
	"$SDL_SOURCE/src/joystick/windows" "$TARGET/joystick/"
cp -a -- "$SDL_SOURCE/src/loadso/dlopen" "$TARGET/loadso/"
cp -a -- "$SDL_SOURCE"/src/sensor/*.{c,h} "$SDL_SOURCE/src/sensor/dummy" \
	"$SDL_SOURCE/src/sensor/windows" "$TARGET/sensor/"
cp -a -- "$SDL_SOURCE"/src/thread/*.{c,h} "$SDL_SOURCE/src/thread/pthread" \
	"$SDL_SOURCE/src/thread/windows" "$TARGET/thread/"
mkdir -p "$TARGET/thread/generic"
cp -a -- "$SDL_SOURCE/src/thread/generic/SDL_syssem.c" \
	"$SDL_SOURCE"/src/thread/generic/SDL_syscond*.{c,h} \
	"$SDL_SOURCE"/src/thread/generic/SDL_sysrwlock*.{c,h} \
	"$SDL_SOURCE/src/thread/generic/SDL_systhread_c.h" "$TARGET/thread/generic/"
cp -a -- "$SDL_SOURCE"/src/timer/*.{c,h} "$SDL_SOURCE/src/timer/unix" \
	"$SDL_SOURCE/src/timer/windows" "$TARGET/timer/"

cp -a -- "$SDL_SOURCE"/src/hidapi/*.{c,h} "$SDL_SOURCE/src/hidapi/AUTHORS.txt" \
	"$SDL_SOURCE/src/hidapi/LICENSE.txt" "$SDL_SOURCE/src/hidapi/LICENSE-bsd.txt" \
	"$SDL_SOURCE/src/hidapi/VERSION" "$TARGET/hidapi/"
for path in hidapi linux mac windows; do
	mkdir -p "$TARGET/hidapi/$path"
	cp -a -- "$SDL_SOURCE"/src/hidapi/"$path"/*.{c,h} "$TARGET/hidapi/$path/"
done

sed -i 's/^VERSION=.*/VERSION=3.2.30/' "$TARGET/update-sdl.sh"
