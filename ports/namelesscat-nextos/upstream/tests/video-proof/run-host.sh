#!/usr/bin/env bash
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)

check_hash() {
  local expected=$1 file=$2 actual
  actual=$(sha256sum -- "$file")
  actual=${actual%% *}
  [[ $actual == "$expected" ]] || {
    printf 'video-proof pin mismatch: %s\n' "$file" >&2
    exit 1
  }
}

check_hash e89fcead5a21102c409f9f48e627fc8471aed1c32a3ec18c8f4cc6ec1478297e \
  "$PORT_DIR/vendor/nxgl/adapters/nxgl_frame_proof_adapter.c"
check_hash c6267cab34e34483a4d9b348da64ccf00704c2472ff9e0ad8010e0d060e6c8b7 \
  "$PORT_DIR/vendor/nxgl/adapters/nxgl_frame_proof_adapter.h"

python3 - "$PORT_DIR" <<'PY'
from pathlib import Path
import json, sys

root = Path(sys.argv[1])
project = json.loads((root / "nxproject.json").read_text())
assert project["nxport"]["video_proof"] == "required"

main = (root / "src/main.c").read_text()
assert main.index("nxgl_frame_proof_launch_receipt();") < main.index("st_egl_init();")
assert main.index("nxgl_frame_proof_publish();") < main.index("st_input_close();")

sdl = (root / "src/egl_sdl.c").read_text()
assert "nxgl_frame_proof_before_present(0, 0);" not in sdl
assert 'getenv("SDL_VIDEO_EGL_DRIVER")' in sdl
assert 'getenv("SDL_VIDEO_GL_DRIVER")' in sdl
assert 'setenv("SDL_VIDEO_EGL_DRIVER", "libEGL.so", 1)' in sdl
assert 'setenv("SDL_VIDEO_GL_DRIVER", "libGLESv2.so", 1)' in sdl
assert 'retrying portable EGL/GLES provider names' in sdl
assert 'raw EGL "' in sdl
assert '"fallback is unsafe on this backend: %s"' in sdl
refresh = sdl.index("SDL_GL_GetDrawableSize(video_window, &drawable_width, &drawable_height);", sdl.index("EGLBoolean st_sdl_swap_buffers"))
proof = sdl.index("nxgl_frame_proof_before_present(drawable_width, drawable_height);", refresh)
present = sdl.index("SDL_GL_SwapWindow(video_window);", proof)
assert refresh < proof < present

raw = (root / "src/egl.c").read_text()
swap = raw.index("static EGLBoolean my_eglSwapBuffers")
assert "nxgl_frame_proof_before_present(0, 0);" not in raw
create = raw.index("static EGLSurface my_eglCreateWindowSurface")
assert raw.index("nxgl_raw_width = physical[0];", create) < swap
helper_start = raw.index("static void nxgl_raw_before_present(void)")
helper_end = raw.index("static void capture_raw_frame_if_requested", helper_start)
helper = raw[helper_start:helper_end]
assert "eglQuerySurface" not in helper
assert "glBindFramebuffer" not in helper
assert "glGetString" not in helper
assert "nxgl_frame_proof_before_present(nxgl_raw_width, nxgl_raw_height);" in helper
proof = raw.index("nxgl_raw_before_present();", swap)
present = raw.index("p_eglSwapBuffers(display, surface)", swap)
assert proof < present

for build in ("build.sh", "build_universal.sh"):
    recipe = (root / build).read_text()
    assert "find vendor/nxgl/adapters" not in recipe
    assert "printf '%s\\n' vendor/nxgl/adapters/nxgl_frame_proof_adapter.c" in recipe
PY

printf 'VIDEO PROOF WIRING PASS\n'
