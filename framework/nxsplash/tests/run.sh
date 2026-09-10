#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
COMPONENT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM HUP

python3 "$COMPONENT_DIR/tools/release-manifest.py" --verify

# The five-second hold is a CONTRACT of the mandatory NEXT OS screen, not a
# tunable. Every test below compiles it down to 20 ms so the suite stays fast,
# which means nothing here would notice the shipped default being changed.
# Pin the source constant itself.
nxsplash_duration=$(sed -n \
  's/^#define NXSPLASH_DURATION_MS \([0-9]*\)u\{0,1\}$/\1/p' \
  "$COMPONENT_DIR/src/nxsplash.c")
if [ "$nxsplash_duration" != 5000 ]; then
  printf 'nxsplash: the mandatory hold is %s ms, not the contracted 5000\n' \
    "${nxsplash_duration:-absent}" >&2
  exit 1
fi
# The fast override must exist only in this test build, never in the source.
if grep -Fq 'define NXSPLASH_DURATION_MS 20' "$COMPONENT_DIR/src/nxsplash.c"
then
  printf 'nxsplash: the test override leaked into the shipped source\n' >&2
  exit 1
fi

gcc \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -DNXSPLASH_DURATION_MS=20 \
  -DNXSPLASH_FBDEV_PATH='"/definitely/not/a/fb0"' \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -Wconversion \
  -o "$TEST_ROOT/nxsplash-test" \
  "$COMPONENT_DIR/src/nxsplash.c" \
  -ldl

gcc \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -Wconversion \
  -o "$TEST_ROOT/render-golden" \
  "$SCRIPT_DIR/render-golden.c" \
  -ldl

gcc \
  -std=gnu11 \
  -D_GNU_SOURCE \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -Wformat=2 \
  -Wshadow \
  -Wstrict-prototypes \
  -Wconversion \
  -o "$TEST_ROOT/test-fbdev" \
  "$SCRIPT_DIR/test-fbdev.c" \
  -ldl
"$TEST_ROOT/test-fbdev"

[ "$("$TEST_ROOT/nxsplash-test" --version)" = "nxsplash 0.1.2" ]
if "$TEST_ROOT/nxsplash-test" >"$TEST_ROOT/usage.out" 2>"$TEST_ROOT/usage.err"; then
  echo "nxsplash test: missing title unexpectedly succeeded" >&2
  exit 1
else
  status=$?
fi
[ "$status" -eq 2 ]

start=$(date +%s)
SDL_VIDEODRIVER=dummy NX_SPLASH_TTY=/definitely/not/a/tty \
  "$TEST_ROOT/nxsplash-test" "Magic Rampage" \
  >"$TEST_ROOT/runtime.out" 2>"$TEST_ROOT/runtime.err"
end=$(date +%s)
[ $((end - start)) -lt 3 ]
grep -Eq '^nxsplash: renderer=(sdl|fbdev|tty|none)' "$TEST_ROOT/runtime.err"

gcc -shared -fPIC -std=gnu11 -Wall -Wextra -Werror \
  -Wl,-soname,libSDL2-2.0.so.0 \
  -o "$TEST_ROOT/libSDL2-2.0.so.0" \
  "$SCRIPT_DIR/fake-sdl-provider.c"
env -u SDL_VIDEO_EGL_DRIVER -u SDL_VIDEO_GL_DRIVER \
  LD_LIBRARY_PATH="$TEST_ROOT" SDL_VIDEODRIVER=KMSDRM \
  NX_SPLASH_TTY=/definitely/not/a/tty \
  "$TEST_ROOT/nxsplash-test" "Provider Retry" \
  >"$TEST_ROOT/provider.out" 2>"$TEST_ROOT/provider.err"
grep -q '^nxsplash: retrying portable EGL/GLES provider names$' \
  "$TEST_ROOT/provider.err"
grep -q '^nxsplash: renderer=sdl driver=KMSDRM duration_ms=20$' \
  "$TEST_ROOT/provider.err"

SDL_VIDEO_EGL_DRIVER=explicit-egl SDL_VIDEO_GL_DRIVER=explicit-gles \
  LD_LIBRARY_PATH="$TEST_ROOT" SDL_VIDEODRIVER=KMSDRM \
  NX_SPLASH_TTY=/definitely/not/a/tty \
  "$TEST_ROOT/nxsplash-test" "Explicit Provider" \
  >"$TEST_ROOT/explicit.out" 2>"$TEST_ROOT/explicit.err"
if grep -q 'portable EGL/GLES provider' "$TEST_ROOT/explicit.err"; then
  echo "nxsplash test: explicit provider was replaced" >&2
  exit 1
fi

SDL_VIDEO_EGL_DRIVER=explicit-egl SDL_VIDEO_GL_DRIVER=explicit-gles \
  LD_LIBRARY_PATH="$TEST_ROOT" SDL_VIDEODRIVER=KMSDRM \
  NX_SPLASH_TTY=/dev/null \
  "$TEST_ROOT/nxsplash-test" "TTY Must Stay Off" \
  >"$TEST_ROOT/tty-off.out" 2>"$TEST_ROOT/tty-off.err"
if grep -q 'renderer=tty' "$TEST_ROOT/tty-off.err"; then
  echo "nxsplash test: TTY fallback activated without diagnostic opt-in" >&2
  exit 1
fi
SDL_VIDEO_EGL_DRIVER=explicit-egl SDL_VIDEO_GL_DRIVER=explicit-gles \
  LD_LIBRARY_PATH="$TEST_ROOT" SDL_VIDEODRIVER=KMSDRM \
  NX_SPLASH_TTY=/dev/null NX_SPLASH_TTY_DIAGNOSTIC=1 \
  "$TEST_ROOT/nxsplash-test" "TTY Diagnostic" \
  >"$TEST_ROOT/tty-on.out" 2>"$TEST_ROOT/tty-on.err"
grep -q '^nxsplash: renderer=tty diagnostic=1 path=/dev/null duration_ms=20$' \
  "$TEST_ROOT/tty-on.err"

if rg -n 'getenv\("NXSPLASH_(DURATION|DISABLE|SKIP)|--(skip|disable)' \
  "$COMPONENT_DIR/src/nxsplash.c"; then
  echo "nxsplash test: public runtime override detected" >&2
  exit 1
fi
for framebuffer_token in \
  'static int open_framebuffer(FramebufferTarget *target)' \
  'static void run_fbdev_splash(FramebufferTarget *target' \
  'draw_software_frame(&target->surface, title, elapsed)' \
  'renderer=fbdev path=/dev/fb0' \
  'NX_SPLASH_TTY_DIAGNOSTIC'; do
  grep -Fq "$framebuffer_token" "$COMPONENT_DIR/src/nxsplash.c" || {
    echo "nxsplash test: graphical fallback lacks $framebuffer_token" >&2
    exit 1
  }
done
if grep -Fq '/sys/class/tty/tty0/active' "$COMPONENT_DIR/src/nxsplash.c"; then
  echo "nxsplash test: automatic active-VT fallback returned" >&2
  exit 1
fi

draw_screen_hash=$(
  sed -n '/^static void draw_screen(/,/^static void run_sdl_splash(/p' \
    "$COMPONENT_DIR/src/nxsplash.c" | sed '$d' | sha256sum | awk '{print $1}'
)
[ "$draw_screen_hash" = \
  5e7fb1a2e957a3be2bdc824f79c09ec9272477b1b49d4154b68446272d79945c ] || {
  echo "nxsplash test: canonical draw_screen changed" >&2
  exit 1
}

while read -r expected width height elapsed extra; do
  case "$expected" in ''|\#*) continue ;; esac
  [ -z "${extra:-}" ] || {
    echo "nxsplash test: malformed golden row" >&2
    exit 1
  }
  output="$TEST_ROOT/golden-${width}x${height}-${elapsed}.rgba"
  "$TEST_ROOT/render-golden" "$width" "$height" "$elapsed" \
    "Magic Rampage" >"$output"
  actual=$(sha256sum "$output" | awk '{print $1}')
  [ "$actual" = "$expected" ] || {
    echo "nxsplash test: visual golden mismatch ${width}x${height} t=$elapsed" >&2
    exit 1
  }
done < "$SCRIPT_DIR/golden-v1.sha256"

python3 - "$TEST_ROOT/golden-640x480-2500.rgba" <<'PY'
import pathlib
import sys

pixels = pathlib.Path(sys.argv[1]).read_bytes()
if len(pixels) != 640 * 480 * 4:
    raise SystemExit("nxsplash test: golden RGBA length mismatch")
palette = {
    bytes(color)
    for color in (
        (18, 22, 29, 255),
        (5, 9, 11, 255),
        (37, 44, 54, 255),
        (77, 232, 151, 255),
        (91, 201, 245, 255),
        (230, 238, 242, 255),
        (135, 153, 159, 255),
    )
}
seen = {pixels[index:index + 4] for index in range(0, len(pixels), 4)}
if not palette <= seen:
    raise SystemExit("nxsplash test: canonical color palette is incomplete")
if any(pixels[index] != 255 for index in range(3, len(pixels), 4)):
    raise SystemExit("nxsplash test: graphical frame contains transparent pixels")
PY
if rg -n '/home/[^/]+|/Users/[^/]+' "$COMPONENT_DIR" \
  --glob '!**/tests/run.sh'; then
  echo "nxsplash test: personal path detected" >&2
  exit 1
fi

echo "nxsplash tests: PASS"
