#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)"
TEST_ROOT="$(mktemp -d)"
ui_pid=
cleanup() {
  if [ -n "$ui_pid" ]; then
    kill "$ui_pid" 2>/dev/null || true
    wait "$ui_pid" 2>/dev/null || true
  fi
  rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT INT TERM HUP

EXPECTED_DRAW_SCREEN_SHA256=ea16719fa09d03e45dd0ea3ae8b97b992e41aefb43c8e4610c8c23dbf1aa4c91
EXPECTED_640_SHA256=f14db68ad6eceedec168b400d31749c4303f24368a4a5a9ef080ea2498e52a7f
EXPECTED_1280_SHA256=154fd7baa5b20d3d80ab10c99ee84cd42ff47921c1e1807e7ee0b680bb673f3c

draw_screen_sha256="$({
  python3 -B - "$PROJECT_ROOT/ui/nxextract_ui.c" <<'PY'
import hashlib
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
start = source.index(b"static void draw_screen(")
end = source.index(b"\n}\n", start) + 3
print(hashlib.sha256(source[start:end]).hexdigest())
PY
} | tr -d '[:space:]')"
[ "$draw_screen_sha256" = "$EXPECTED_DRAW_SCREEN_SHA256" ] || {
  echo "NXExtract visual gate: approved draw_screen changed" >&2
  exit 1
}

cc \
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
  -o "$TEST_ROOT/nxextract-ui" \
  "$PROJECT_ROOT/ui/nxextract_ui.c" \
  -ldl

capture_and_check() {
  local size=$1 expected=$2 output="$TEST_ROOT/$1.ppm" actual
  NXEXTRACT_TEST_CAPTURE_PPM="$output" \
  NXEXTRACT_TEST_CAPTURE_SIZE="$size" \
    "$TEST_ROOT/nxextract-ui" \
      "$TEST_ROOT/no-progress" "$TEST_ROOT/no-stop" "$TEST_ROOT/no-ready" \
      "NXEXTRACT VISUAL BASELINE" "1.2.6" \
      >/dev/null
  actual="$(sha256sum "$output" | awk '{print $1}')"
  [ "$actual" = "$expected" ] || {
    echo "NXExtract visual gate: $size golden changed ($actual)" >&2
    exit 1
  }
}

capture_and_check 640x480 "$EXPECTED_640_SHA256"
capture_and_check 1280x720 "$EXPECTED_1280_SHA256"

cc -shared -fPIC -std=gnu11 -Wall -Wextra -Werror \
  -Wl,-soname,libSDL2-2.0.so.0 \
  -o "$TEST_ROOT/libSDL2-2.0.so.0" \
  "$SCRIPT_DIR/fake-sdl-provider.c"

run_provider_case() {
  local name=$1 ready="$TEST_ROOT/$1.ready" stop="$TEST_ROOT/$1.stop"
  local output="$TEST_ROOT/$1.out" error="$TEST_ROOT/$1.err" count
  shift
  env "$@" LD_LIBRARY_PATH="$TEST_ROOT" SDL_VIDEODRIVER=KMSDRM \
    "$TEST_ROOT/nxextract-ui" \
      "$TEST_ROOT/no-progress" "$stop" "$ready" \
      "PROVIDER FIXTURE" "1.2.9" >"$output" 2>"$error" &
  ui_pid=$!
  count=0
  while [ ! -s "$ready" ] && kill -0 "$ui_pid" 2>/dev/null; do
    count=$((count + 1))
    [ "$count" -lt 200 ] || break
    sleep 0.01
  done
  [ "$(cat "$ready" 2>/dev/null || true)" = "visible=sdl" ] || {
    echo "NXExtract visual gate: provider fixture did not reach SDL" >&2
    return 1
  }
  : >"$stop"
  wait "$ui_pid"
  ui_pid=
}

run_provider_case portable \
  -u SDL_VIDEO_EGL_DRIVER -u SDL_VIDEO_GL_DRIVER
grep -Fqx 'nxextract-ui: retrying portable EGL/GLES provider names' \
  "$TEST_ROOT/portable.err"
grep -Fqx 'nxextract-ui: recovered with portable EGL/GLES provider names' \
  "$TEST_ROOT/portable.err"

run_provider_case explicit \
  SDL_VIDEO_EGL_DRIVER=explicit-egl SDL_VIDEO_GL_DRIVER=explicit-gles \
  NXEXTRACT_FAKE_ACCEPT_EXPLICIT=1
if grep -Fq 'portable EGL/GLES provider' "$TEST_ROOT/explicit.err"; then
  echo "NXExtract visual gate: explicit provider was replaced" >&2
  exit 1
fi

grep -Fq 'FALLBACK_RETRIES_PER_DRIVER = 6' \
  "$PROJECT_ROOT/ui/nxextract_ui.c"
grep -Fq 'if (attempt == 6 && inherited_mode)' \
  "$PROJECT_ROOT/ui/nxextract_ui.c"
grep -Fq 'usleep(500000)' "$PROJECT_ROOT/ui/nxextract_ui.c"
grep -Fq 'enable_portable_provider_retry()' \
  "$PROJECT_ROOT/ui/nxextract_ui.c"
grep -Fq 'publish_ready(ready_path, "fbdev")' \
  "$PROJECT_ROOT/ui/nxextract_ui.c"
if grep -Fq 'publish_ready(ready_path, "tty")' \
    "$PROJECT_ROOT/ui/nxextract_ui.c"; then
  echo "NXExtract visual gate: TTY can still attest public readiness" >&2
  exit 1
fi

echo "NXExtract visual identity tests: PASS"
