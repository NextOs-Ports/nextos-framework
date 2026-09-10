#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/asm2-gles-oes-test.XXXXXX")
trap 'rm -rf -- "$TMP_ROOT"' EXIT INT TERM

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || {
  printf 'gles_oes_alias_test: FAIL: missing C compiler: %s\n' "$CC" >&2
  exit 1
}

grep -Fq \
  'ASM2_IMPORT("glMapBufferOES", asm2_glMapBufferOES)' \
  "$PORT_DIR/src/imports.c"
grep -Fq \
  'ASM2_IMPORT("glUnmapBufferOES", asm2_glUnmapBufferOES)' \
  "$PORT_DIR/src/imports.c"
grep -Fq 'ASM2_IMPORT("glMapBufferOES", 0)' "$PORT_DIR/src/imports.c"
grep -Fq 'ASM2_IMPORT("glUnmapBufferOES", 0)' "$PORT_DIR/src/imports.c"

"$CC" \
  -std=gnu11 -D_GNU_SOURCE -DASM2_TEST_EGL_FALLBACK \
  -I"$PORT_DIR/src" -O2 \
  -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -c "$PORT_DIR/src/platform_shims.c" \
  -o "$TMP_ROOT/platform_shims.o"
"$CC" \
  -std=gnu11 -D_GNU_SOURCE -I"$PORT_DIR/src" -O2 \
  -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  "$SCRIPT_DIR/gles_oes_alias_test.c" "$TMP_ROOT/platform_shims.o" \
  -Wl,--gc-sections -o "$TMP_ROOT/gles_oes_alias_test"

for mode in sdl-core egl-oes dlsym-core; do
  "$TMP_ROOT/gles_oes_alias_test" "$mode"
done
