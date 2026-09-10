#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V3-GRAPHICS-01 host gate (standalone, additive to run-m13-host.sh).
#
# What it proves, all hermetic (fake providers, no display/GPU/device):
#   1. EGLConfig requirement API: the DEFAULT request is all-don't-care --
#      RGBA8888 stays a per-adapter declaration (Huntdown), never a global.
#   2. Single-clean-retry contract: at most ONE retry after a real failure,
#      the second failure is terminal.
#   3. Mirrored candidate scenarios: a dead/orphan candidate ordered before a
#      live one loses in BOTH directions; the receipt names the winner and
#      every loser's reason; a healthy first candidate proves the later rung
#      is never probed (order recorded in the trace).
#   4. A provider that accepts every call but never draws is failed by the
#      frame proof, and the VIDEO receipt now names WHERE it sampled
#      (sample_point=..., an APPENDED field only).
#   5. No device/brand name is ever a selection condition in src/ or include/.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXGL=$(cd -- "$HERE/.." && pwd -P)
NXLOADER=$(cd -- "$NXGL/../nxloader" && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxgl-v3-graphics.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "nxgl_v3_graphics=FAIL $*" >&2; exit 1; }

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes -Wconversion -Wsign-conversion -Wcast-qual)

# ---------------------------------------------------------------- 1 + 2
# Pure units, compiled -Werror clean with BOTH compilers when clang exists.
compilers=("$CC")
if command -v clang >/dev/null 2>&1 && [ "$CC" != clang ]; then
  compilers+=(clang)
fi
for compiler in "${compilers[@]}"; do
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" \
    -o "$WORK/config-request-$compiler" \
    "$HERE/test_v3_config_request.c" "$NXGL/src/nxgl_config_request.c" ||
    fail "config-request nao compilou limpo com $compiler"
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" \
    -o "$WORK/retry-contract-$compiler" \
    "$HERE/test_v3_retry_contract.c" "$NXGL/src/nxgl_retry_contract.c" ||
    fail "retry-contract nao compilou limpo com $compiler"
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" \
    -o "$WORK/quality-$compiler" \
    "$HERE/test_v3_quality.c" "$NXGL/src/nxgl_quality.c" ||
    fail "quality nao compilou limpo com $compiler"
  "$WORK/config-request-$compiler" || fail "config-request ($compiler)"
  "$WORK/retry-contract-$compiler" || fail "retry-contract ($compiler)"
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" \
    -o "$WORK/graphics-contract-$compiler" \
    "$HERE/test_v3_graphics_contract.c" "$NXGL/src/nxgl_graphics_contract.c" ||
    fail "graphics-contract nao compilou limpo com $compiler"
  "$WORK/graphics-contract-$compiler" || fail "graphics-contract ($compiler)"
  "$compiler" "${STRICT[@]}" -I "$NXGL/include" \
    -o "$WORK/single-channel-$compiler" \
    "$HERE/test_v3_single_channel.c" "$NXGL/src/nxgl_single_channel.c" ||
    fail "single-channel nao compilou limpo com $compiler"
  "$WORK/single-channel-$compiler" || fail "single-channel ($compiler)"
  "$WORK/quality-$compiler" || fail "quality ($compiler)"
done

# ---------------------------------------------------------------- 3
# Mirrored candidate scenarios over the real nxgl_gles1 resolver.
GLES_INCLUDE=${NXGL_GLES1_TEST_INCLUDE:-}
if [ -z "$GLES_INCLUDE" ]; then
  for candidate in \
    "$HOME"/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain/aarch64-libreelec-linux-gnu/sysroot/usr/include \
    /usr/include; do
    if [ -f "$candidate/GLES/gl.h" ]; then
      GLES_INCLUDE=$candidate
      break
    fi
  done
fi
candidates_result=SKIP
if [ -n "$GLES_INCLUDE" ]; then
  "$CC" -shared -fPIC -o "$WORK/live.so" "$HERE/fake_gles1_live.c"
  "$CC" -shared -fPIC -o "$WORK/dead.so" "$HERE/fake_gles1_dead.c"
  "$CC" "${STRICT[@]}" -D_POSIX_C_SOURCE=200809L \
    -I "$NXGL/include" -I "$GLES_INCLUDE" \
    -o "$WORK/candidates" "$HERE/test_v3_candidates.c" \
    "$NXGL/src/nxgl_gles1.c" -ldl ||
    fail "harness de candidatos nao compilou limpo"

  expect() { [ "$2" = "$1" ] || fail "$3: esperado [$1], veio [$2]"; }

  # Scenario A (ROCKNIX shape): dead/orphan BLOB ordered BEFORE the live
  # versioned Mesa. Measurement must pick the Mesa; the trace names the dead
  # blob's reason and records the probe order.
  mkdir -p "$WORK/a"
  cp "$WORK/dead.so" "$WORK/a/libmali.so"
  cp "$WORK/live.so" "$WORK/a/libGLESv1_CM.so.1"
  out=$(LD_LIBRARY_PATH="$WORK/a" "$WORK/candidates")
  expect "rc=0 provider=libGLESv1_CM.so.1 liveness=ok trace=[RTLD_DEFAULT:incomplete libmali.so:dead libMali.so:absent libGLES_mali.so:absent libGLESv1_CM.so:absent libmali.so.1:absent libGLESv1_CM.so.1:live]" \
    "$out" "cenario A (blob morto antes da Mesa viva)"

  # Scenario B (dArkOS shape): dead MESA ordered BEFORE the live blob.
  mkdir -p "$WORK/b"
  cp "$WORK/dead.so" "$WORK/b/libGLESv1_CM.so"
  cp "$WORK/live.so" "$WORK/b/libmali.so.1"
  out=$(LD_LIBRARY_PATH="$WORK/b" "$WORK/candidates")
  expect "rc=0 provider=libmali.so.1 liveness=ok trace=[RTLD_DEFAULT:incomplete libmali.so:absent libMali.so:absent libGLES_mali.so:absent libGLESv1_CM.so:dead libmali.so.1:live]" \
    "$out" "cenario B (Mesa morta antes do blob vivo)"

  # Healthy-first: with a LIVE first candidate, the later rung must never be
  # probed -- the trace ENDS at the winner, and the second provider present
  # in the directory never shows up in it.
  mkdir -p "$WORK/c"
  cp "$WORK/live.so" "$WORK/c/libmali.so"
  cp "$WORK/live.so" "$WORK/c/libGLESv1_CM.so.1"
  out=$(LD_LIBRARY_PATH="$WORK/c" "$WORK/candidates")
  expect "rc=0 provider=libmali.so liveness=ok trace=[RTLD_DEFAULT:incomplete libmali.so:live]" \
    "$out" "saudavel primeiro (degrau extra nunca invocado)"
  case $out in
    *libGLESv1_CM.so.1*) fail "degrau extra foi invocado: $out" ;;
  esac
  candidates_result=PASS
else
  echo "nxgl_v3_candidates=SKIP sem GLES/gl.h no host" >&2
fi

# ---------------------------------------------------------------- 4
# Provider that accepts calls but does not draw -> frame proof fails it.
"$CC" -Wall -Wextra -Werror -O1 -o "$WORK/nodraw" \
  "$HERE/test_v3_frame_proof_nodraw.c" \
  "$NXGL/adapters/nxgl_frame_proof_adapter.c" -ldl ||
  fail "harness nodraw nao compilou limpo"

# Graphics-contract runtime adapter: measures the obtained context (glGetString
# authoritative), refuses 1x1, and fails a desktop-GL context for a GLES
# contract before shaders. Compiled with the adapter (GL-touching) flag set.
for compiler in "${compilers[@]}"; do
  "$compiler" -Wall -Wextra -Werror -O1 -I "$NXGL/include" -I "$NXGL/adapters" \
    -o "$WORK/graphics-adapter-$compiler" \
    "$HERE/test_v3_graphics_adapter.c" \
    "$NXGL/adapters/nxgl_graphics_contract_adapter.c" \
    "$NXGL/src/nxgl_graphics_contract.c" \
    "$NXGL/src/nxgl_graphics_present_gate.c" -lpthread -ldl ||
    fail "graphics-adapter nao compilou limpo com $compiler"
  "$WORK/graphics-adapter-$compiler" || fail "graphics-adapter ($compiler)"
  # Single-channel opt-in runtime adapter (measure caps, per-texture route
  # tracking, R->LA duplication, RGBA untouched). GL-touching flag set.
  "$compiler" -Wall -Wextra -Werror -O1 -I "$NXGL/include" -I "$NXGL/adapters" \
    -o "$WORK/single-channel-adapter-$compiler" \
    "$HERE/test_v3_single_channel_adapter.c" \
    "$NXGL/adapters/nxgl_single_channel_adapter.c" \
    "$NXGL/src/nxgl_single_channel.c" -ldl ||
    fail "single-channel-adapter nao compilou limpo com $compiler"
  "$WORK/single-channel-adapter-$compiler" ||
    fail "single-channel-adapter ($compiler)"

  # Real nxloader registry integration. Compile nxloader with its own strict
  # warning contract, and nxgl/provider/test code with nxgl's stronger one.
  compiler_tag=${compiler##*/}
  nxloader_extra_warnings=()
  if [ "$compiler_tag" = clang ]; then
    # glibc's C99 memchr macro uses an internal C11 _Generic solely for const
    # preservation; this is a system-header extension, not nxloader code.
    nxloader_extra_warnings=(-Wno-c11-extensions)
  fi
  nxloader_objects=()
  for source in \
    nxloader.c nxloader_elf32.c nxloader_elf64.c nxloader_hooks.c \
    nxloader_protect.c nxloader_registry.c; do
    object="$WORK/${source%.c}-$compiler_tag.o"
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
      "${nxloader_extra_warnings[@]}" \
      -I "$NXLOADER/include" -I "$NXLOADER/src" \
      -c "$NXLOADER/src/$source" -o "$object" ||
      fail "nxloader provider dependency nao compilou: $source ($compiler)"
    nxloader_objects+=("$object")
  done
  provider_objects=()
  for unit in \
    "$HERE/test_v3_nxloader_provider.c" \
    "$NXGL/adapters/nxgl_nxloader_provider.c" \
    "$NXGL/adapters/nxgl_graphics_contract_adapter.c" \
    "$NXGL/src/nxgl_graphics_contract.c" \
    "$NXGL/src/nxgl_graphics_present_gate.c"; do
    object="$WORK/$(basename "${unit%.c}")-$compiler_tag.o"
    "$compiler" "${STRICT[@]}" -D_POSIX_C_SOURCE=200809L \
      -I "$NXGL/include" -I "$NXGL/adapters" \
      -I "$NXLOADER/include" -I "$NXLOADER/src" \
      -c "$unit" -o "$object" ||
      fail "nxgl nxloader provider nao compilou: $(basename "$unit") ($compiler)"
    provider_objects+=("$object")
  done
  "$compiler" -o "$WORK/nxloader-provider-$compiler_tag" \
    "${provider_objects[@]}" "${nxloader_objects[@]}" -pthread -ldl ||
    fail "nxgl nxloader provider nao linkou ($compiler)"
  "$WORK/nxloader-provider-$compiler_tag" ||
    fail "nxgl nxloader provider ($compiler)"
done

run_fp() { # $1 modo
  env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=1 \
    "$WORK/nodraw" "$1"
}

out=$(run_fp nodraw)
grep -q 'gl: frame proof verdict=BLACK' <<<"$out" ||
  fail "nodraw: provedor que aceita chamadas sem desenhar nao foi reprovado: $out"
grep -q 'verdict=BLACK reason=all-black sample_point=before-present' <<<"$out" ||
  fail "nodraw: recibo VIDEO sem razao/ponto de amostra: $out"

# 0.3.5: the fatal flag is lock-free -- a frame-loop poll during a legitimate
# render-thread readback must never read as fatal (Nameless Cat closed a healthy
# game with status 72 at frame 32 on 0.3.4). Two threads, big frames, then the
# real conclusive fatal must still arm once.
"$CC" -std=c99 -Wall -Wextra -Werror -O1 -pthread -D_POSIX_C_SOURCE=200809L \
  -o "$WORK/fatal-race" "$HERE/test_frame_proof_fatal_race.c" \
  "$NXGL/adapters/nxgl_frame_proof_adapter.c" -ldl ||
  fail "harness fatal-race nao compilou limpo"
out=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=1 "$WORK/fatal-race" 2>&1) ||
  fail "fatal-race: $out"
grep -q 'nxgl_frame_proof_fatal_race=PASS' <<<"$out" || fail "fatal-race sem PASS: $out"

out=$(run_fp draws)
grep -q 'verdict=OK reason=none sample_point=before-present' <<<"$out" ||
  fail "draws: recibo VIDEO errado: $out"

out=$(run_fp after)
grep -q 'sample_point=after-present' <<<"$out" ||
  fail "after: ponto de amostra after-present nao registrado: $out"

out=$(run_fp legacy)
grep -q 'sample_point=unspecified' <<<"$out" ||
  fail "legacy: nxgl_frame_proof_sample devia registrar unspecified: $out"
# A legacy unspecified read remains visible in diagnostics but cannot claim it
# came from the framebuffer that was about to be presented.
grep -q 'VIDEO: window=64x64 driver=KMSDRM renderer=Fake Healthy GPU gles="OpenGL ES 2.0 fake" frame_proof=100.0% verdict=INCONCLUSIVE reason=presentation-point-unproved' <<<"$out" ||
  fail "legacy: sample sem fronteira de present alegou OK (regressao!): $out"

# ---------------------------------------------------------------- 5
# No device/brand name as a selection condition.
#
# Enforcement, pragmatically:
#  (a) The files ADDED by V3 must not contain the tokens AT ALL (not even in
#      comments).
#  (b) Across all of src/ + include/, NON-COMMENT occurrences (comments
#      stripped with gcc -fpreprocessed) must not exceed the frozen baseline
#      below. Each baseline line is a measured fact, not name-keyed selection:
#        src/nxgl_gles1.c (4)      dlopen FILENAME candidates -- every one is
#                                  still proven by per-candidate liveness
#                                  measurement before selection;
#        src/nxgl_diagnostics.c(1) the exact renderer-string tuple MEASURED on
#                                  real hardware 2026-08-16 (bridge gate);
#        src/nxgl_present.c (1) +
#        include/nxgl.h (1)        the caller-DECLARED OSD quirk enum -- the
#                                  adapter opts in; nxgl never infers it.
#      Growth anywhere (or a new file matching) fails the gate.
tokens='mali|panfrost|rk35|amlogic'
for new_file in \
  include/nxgl_config_request.h src/nxgl_config_request.c \
  include/nxgl_retry_contract.h src/nxgl_retry_contract.c; do
  if grep -qiE "$tokens" "$NXGL/$new_file"; then
    fail "arquivo novo do V3 menciona nome de device/marca: $new_file"
  fi
done
baseline_for() {
  case $1 in
    src/nxgl_gles1.c) echo 4 ;;
    # 4 candidatos de FILENAME para dlopen (cada um provado por medicao de
    # vivacidade antes da selecao, como no gles1) + 2 casamentos falsos do
    # parametro GLboolean "normalized" com o token 'mali'.
    src/nxgl_gles2.c) echo 6 ;;
    # 1 casamento falso do "normalized" no prototipo de glVertexAttribPointer.
    include/nxgl_gles2.h) echo 1 ;;
    src/nxgl_diagnostics.c) echo 1 ;;
    src/nxgl_present.c) echo 1 ;;
    include/nxgl.h) echo 1 ;;
    *) echo 0 ;;
  esac
}
for f in "$NXGL"/src/*.c "$NXGL"/src/*.h "$NXGL"/include/*.h; do
  rel=${f#"$NXGL"/}
  count=$(gcc -fpreprocessed -dD -E -P "$f" 2>/dev/null |
    grep -icE "$tokens" || true)
  allowed=$(baseline_for "$rel")
  if [ "$count" -gt "$allowed" ]; then
    fail "nome de device/marca fora da baseline em $rel: $count > $allowed (nunca selecionar por nome; selecionar por MEDICAO)"
  fi
done

echo "nxgl_v3_graphics=PASS config_request=PASS retry_contract=PASS quality=PASS graphics_contract=PASS single_channel=PASS graphics_adapter=PASS nxloader_provider=PASS single_channel_adapter=PASS candidates=$candidates_result frame_proof_nodraw=PASS fatal_race=PASS sample_point=PASS brand_name_gate=PASS"
