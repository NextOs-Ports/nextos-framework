#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V4 host gate for nxgl 0.3.0.
#
# 1. V4-DISPLAY-01: content rect, exact inverse input map, finite policies,
#    the no-op default and every negative -- pure, no GL, no device.
# 2. V4-GRAPHICS-03: the EGL binding contract with injected dl ops, covering
#    the GLVND split that crashed OTR 1.0.3 on ROCKNIX, monolithic Mali, the
#    already-global path, and all the refusals.
# 3. A REAL ELF audit: the EGL inventory is read out of a real shared object,
#    an undeclared import fails, and DT_NEEDED libEGL is refused.
# 4. Static audit: no device, CFW, GPU or game name may be a condition in the
#    new sources.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXGL=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxgl-v4-host.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "nxgl_v4=FAIL $*" >&2; exit 1; }

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes -Wconversion -Wsign-conversion -Wcast-qual)

compilers=("$CC")
if command -v clang >/dev/null 2>&1 && [ "$CC" != clang ]; then
  compilers+=(clang)
fi

for compiler in "${compilers[@]}"; do
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" -o "$WORK/display-$compiler" \
    "$HERE/test_v4_display.c" "$NXGL/src/nxgl_display.c" ||
    fail "nxgl_display did not compile cleanly with $compiler"
  "$WORK/display-$compiler" || fail "nxgl_display ($compiler)"
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" -o "$WORK/egl-$compiler" \
    "$HERE/test_v4_egl_binding.c" "$NXGL/src/nxgl_egl_binding.c" ||
    fail "nxgl_egl_binding did not compile cleanly with $compiler"
  "$WORK/egl-$compiler" || fail "nxgl_egl_binding ($compiler)"
done

# ----------------------------------------------- V4-GRAPHICS-04 present gate
# The pure post-first-present state machine and the lifecycle adapter against
# an injected fake SDL/GL, under GCC, Clang, ASAN and UBSAN. The fake models
# the Wayland lifecycle: the drawable stays 1x1 until the guest's REAL swap.
for compiler in "${compilers[@]}"; do
  for san in "" "-fsanitize=address -fno-omit-frame-pointer" \
             "-fsanitize=undefined -fno-omit-frame-pointer"; do
    tag="$compiler${san:+-${san#-fsanitize=}}"; tag=${tag%% *}
    # shellcheck disable=SC2086
    "$compiler" "${STRICT[@]}" $san -I "$NXGL/include" \
      -o "$WORK/present-gate-$tag" \
      "$HERE/test_v4_graphics_present_gate.c" \
      "$NXGL/src/nxgl_graphics_present_gate.c" \
      "$NXGL/src/nxgl_graphics_contract.c" ||
      fail "present gate did not compile cleanly ($compiler $san)"
    "$WORK/present-gate-$tag" || fail "present gate ($compiler $san)"
    # shellcheck disable=SC2086
    "$compiler" "${STRICT[@]}" $san -D_POSIX_C_SOURCE=200809L \
      -I "$NXGL/include" -I "$NXGL/adapters" \
      -o "$WORK/lifecycle-adapter-$tag" \
      "$HERE/test_v4_graphics_lifecycle_adapter.c" \
      "$NXGL/adapters/nxgl_graphics_contract_adapter.c" \
      "$NXGL/src/nxgl_graphics_present_gate.c" \
      "$NXGL/src/nxgl_graphics_contract.c" -lpthread -ldl ||
      fail "lifecycle adapter did not compile cleanly ($compiler $san)"
    "$WORK/lifecycle-adapter-$tag" || fail "lifecycle adapter ($compiler $san)"
  done
done

# ----------------------------------------------------------------- real ELF
cat > "$WORK/guest.c" <<'GUEST'
extern void *eglGetCurrentContext(void);
extern void *eglGetCurrentDisplay(void);
extern int eglSwapBuffers(void *display, void *surface);
void *nx_guest_frame(void *display, void *surface) {
  if (eglGetCurrentContext() == 0) { return eglGetCurrentDisplay(); }
  return (void *)(long)eglSwapBuffers(display, surface);
}
GUEST
"$CC" -shared -fPIC -o "$WORK/guest.so" "$WORK/guest.c" ||
  fail "the ELF fixture guest could not be built"

audit() { python3 -B "$NXGL/tools/nxgl-egl-audit.py" "$@"; }

audit "$WORK/guest.so" --forbid-needed-libegl --max-glibc 2.30 \
  --declared eglGetCurrentContext --declared eglGetCurrentDisplay \
  --declared eglSwapBuffers > "$WORK/audit-ok.json" ||
  fail "the exact declared EGL inventory was refused"
grep -Fq '"egl_jump_slot"' "$WORK/audit-ok.json" ||
  fail "the audit did not report the relocation kind"
grep -Fq '"eglSwapBuffers"' "$WORK/audit-ok.json" ||
  fail "the audit lost a real EGL import"

if audit "$WORK/guest.so" --declared eglGetCurrentContext \
     > "$WORK/audit-undeclared.json" 2>&1; then
  fail "an undeclared EGL import passed the audit"
fi
grep -Fq 'undeclared EGL imports' "$WORK/audit-undeclared.json" ||
  fail "the audit did not name the undeclared imports"

if audit "$WORK/guest.so" --declared eglGetCurrentContext \
     --declared eglGetCurrentDisplay --declared eglSwapBuffers \
     --declared eglCreatePbufferSurface > "$WORK/audit-extra.json" 2>&1; then
  fail "a declared import the guest never uses passed the audit"
fi

cat > "$WORK/fakeegl.c" <<'FAKE'
void *eglGetCurrentContext(void) { return 0; }
void *eglGetCurrentDisplay(void) { return 0; }
int eglSwapBuffers(void *display, void *surface) { (void)display; (void)surface; return 1; }
FAKE
"$CC" -shared -fPIC -o "$WORK/libEGL.so.1" "$WORK/fakeegl.c" ||
  fail "the fake EGL provider could not be built"
"$CC" -shared -fPIC -o "$WORK/linked.so" "$WORK/guest.c" \
  -L"$WORK" -l:libEGL.so.1 -Wl,-rpath,"$WORK" ||
  fail "the DT_NEEDED fixture could not be linked"
if audit "$WORK/linked.so" --forbid-needed-libegl > "$WORK/audit-needed.json" 2>&1
then
  fail "DT_NEEDED libEGL passed the universal-executable audit"
fi
grep -Fq 'DT_NEEDED libEGL' "$WORK/audit-needed.json" ||
  fail "the audit did not name the forbidden DT_NEEDED"

# ------------------------------------------------------------- static audit
# Selection is by measurement only. A device, CFW, GPU or game name must never
# appear as a condition in the new sources.
for token in mali Mali MALI panfrost Panfrost rocknix ROCKNIX ArkOS arkos \
             muos muOS EmuELEC emuelec NextOS RG40 R36 rk3326 Adreno; do
  ! grep -Fq -- "$token" "$NXGL/src/nxgl_display.c" ||
    fail "nxgl_display.c mentions '$token'"
  ! grep -Fq -- "$token" "$NXGL/src/nxgl_egl_binding.c" ||
    fail "nxgl_egl_binding.c mentions '$token'"
  ! grep -Fq -- "$token" "$NXGL/include/nxgl_display.h" ||
    fail "nxgl_display.h mentions '$token'"
  ! grep -Fq -- "$token" "$NXGL/src/nxgl_graphics_present_gate.c" ||
    fail "nxgl_graphics_present_gate.c mentions '$token'"
  ! grep -Fq -- "$token" "$NXGL/include/nxgl_graphics_present_gate.h" ||
    fail "nxgl_graphics_present_gate.h mentions '$token'"
done
# The present gate core is pure: no dl, no environment, no GL, no clock, no
# external stat, no I/O beyond formatting into caller buffers.
! grep -Eq 'dlopen|dlsym|getenv|fopen|open\(|system\(|popen|clock_gettime|gl[A-Z]|SDL_|\bstat\(' \
    "$NXGL/src/nxgl_graphics_present_gate.c" ||
  fail "nxgl_graphics_present_gate.c is not a pure module"
# The V4 additions never fabricate frames: the adapter must not resolve or
# call any clear/draw/swap entry point.
! grep -Eq 'glClear|glDraw|SwapWindow|eglSwapBuffers|SwapBuffers' \
    "$NXGL/adapters/nxgl_graphics_contract_adapter.c" ||
  fail "the graphics adapter fabricates clear/draw/swap"
# The binding must never open the global namespace before the proof, and the
# display module must never touch GL or the environment.
! grep -Eq 'RTLD_GLOBAL|dlopen|getenv' "$NXGL/src/nxgl_egl_binding.c" ||
  fail "nxgl_egl_binding.c performs its own dl/environment effects"
! grep -Eq 'gl[A-Z]|getenv|fopen|SDL_' "$NXGL/src/nxgl_display.c" ||
  fail "nxgl_display.c is not a pure module"

echo "nxgl_v4=PASS display=1 egl_binding=1 present_gate=1 lifecycle_adapter=1 elf_audit=1 static=1"
