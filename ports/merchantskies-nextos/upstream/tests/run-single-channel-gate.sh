#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Merchant font/atlas gate: canonical policy is additive; the proven Mali-450
# TexStorage implementation and local (R,R) bridge remain immutable.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT=$(cd -- "$HERE/.." && pwd -P)
SRC="$PORT/src"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/merchant-single-channel.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "merchant_single_channel_gate=FAIL $*" >&2
  exit 1
}

hash_is() {
  local file=$1
  local expected=$2
  local actual
  actual=$(sha256sum "$file" | awk '{print $1}')
  [ "$actual" = "$expected" ] ||
    fail "source-lock $(basename -- "$file") expected=$expected actual=$actual"
}

[ -f "$SRC/egl.c" ] || fail "missing $SRC/egl.c"
[ -f "$SRC/gles3.c" ] || fail "missing $SRC/gles3.c"

# Whole-file lock for the proven ES2 compatibility backend.
hash_is "$SRC/gles3.c" \
  b0de8c3736d509c637b74318a260946112a3c1fc4675f3c0e94a031d9407c36d
hash_is "$SRC/egl_sdl.c" \
  ed9f4cecd74c1b3e711f48f92e0a9dbf1b86ad16f4b2d51ee4c1c3d70613a972
hash_is "$SRC/egl_sdl.h" \
  803e02dd2ca782792ffa5166bce546623d04ff5bc7f9fae645867b280af1e00d

# Deterministic half-open extraction: include the critical function(s), stop
# immediately before the next function declaration. Marker cardinality is
# checked so an accidental duplicate cannot weaken the hash lock.
[ "$(grep -Ec '^static int format_is_single_channel\(' "$SRC/egl.c")" -eq 1 ] ||
  fail "format_is_single_channel marker cardinality"
[ "$(grep -Ec '^static void my_glTexImage2D\(' "$SRC/egl.c")" -eq 1 ] ||
  fail "my_glTexImage2D marker cardinality"
awk '
  /^static int format_is_single_channel\(/ { emit = 1 }
  /^static void my_glTexImage2D\(/ { emit = 0 }
  emit
' "$SRC/egl.c" >"$WORK/egl-r8-la-golden.c"
hash_is "$WORK/egl-r8-la-golden.c" \
  3041482c7d7a49dd39e60a563e74c0e1a0bd5ac57e0e8b930756f4d8dedea86d

[ "$(grep -Ec '^static void gl3_TexStorage2D\(' "$SRC/gles3.c")" -eq 1 ] ||
  fail "gl3_TexStorage2D marker cardinality"
[ "$(grep -Ec '^static void gl3_TexStorage3D\(' "$SRC/gles3.c")" -eq 1 ] ||
  fail "gl3_TexStorage3D marker cardinality"
awk '
  /^static void gl3_TexStorage2D\(/ { emit = 1 }
  /^static void gl3_TexStorage3D\(/ { emit = 0 }
  emit
' "$SRC/gles3.c" >"$WORK/gles3-texstorage2d-golden.c"
hash_is "$WORK/gles3-texstorage2d-golden.c" \
  b29b1fa8644fab65d953a95f4de4039bf76221415027c219b27ab81bb4b1dd0a

converter_calls=$(grep -Ec \
  '^[[:space:]]*expanded[[:space:]]*=[[:space:]]*expand_r8_to_luminance_alpha\(' \
  "$SRC/egl.c" || true)
[ "$converter_calls" -eq 2 ] ||
  fail "golden (R,R) converter calls expected=2 actual=$converter_calls"
converter_mentions=$(grep -o 'expand_r8_to_luminance_alpha' "$SRC/egl.c" |
  wc -l | tr -d '[:space:]')
[ "$converter_mentions" -eq 3 ] ||
  fail "golden converter symbol mentions expected=3 actual=$converter_mentions"

if grep -Fq 'my_glTexStorage2D' "$SRC/egl.c"; then
  fail "forbidden Merchant TexStorage wrapper"
fi
if tr -d '[:space:]' <"$SRC/egl.c" |
    grep -Fq 'strcmp(name,"glTexStorage2D")'; then
  fail "forbidden Merchant glTexStorage2D name interception"
fi
if grep -Fq 'nxgl_single_channel_adapter_convert' "$SRC/egl.c"; then
  fail "canonical converter cannot replace Merchant proven local conversion"
fi

# The local gate intentionally refuses nxgl 0.2.16: coverage semantics are an
# additive 0.2.17 dependency and must be vendored into this exact port first.
grep -Fq 'NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT' \
  "$SRC/nxgl_single_channel.h" ||
  fail "dependency nxgl single-channel 0.2.17 not vendored"
grep -Fq 'nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous' \
  "$SRC/nxgl_single_channel_adapter.h" ||
  fail "dependency nxgl adapter 0.2.17 not vendored"

# Exact vendored component identity from framework commit f721af2. A port may
# select the new semantic, but it may not carry a private fork of the policy.
hash_is "$SRC/nxgl_single_channel.c" \
  52a16203241b087a04df32747c9c77467e2f591c5e0834ec977f63b86a7ed98b
hash_is "$SRC/nxgl_single_channel.h" \
  11be4e270db0745cea46abdf879baf5fed9843545f77b758d8f7774c38cd2327
hash_is "$SRC/nxgl_single_channel_adapter.c" \
  4725550aa10978dbe8ffa120a6c2ae8522226d9d58a2e713dfde4b721aa8624e
hash_is "$SRC/nxgl_single_channel_adapter.h" \
  d604ce815b6fd354d1a27ea5a2272120deeed9acce7fbe8a9c6ef204232acd64

grep -Fq '.semantic = NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT' "$SRC/egl.c" ||
  fail "Merchant runtime did not opt in to RED_COVERAGE_COMPAT"
grep -Fq 'nxgl_single_channel_adapter_measure_context(' "$SRC/egl.c" ||
  fail "Merchant runtime is not using canonical capability measurement"
grep -Fq 'return st_sdl_current_context_token();' "$SRC/egl.c" ||
  fail "SDL-owned context identity is not wired to canonical measurement"
grep -Fq 'texstorage=caller-owned fallback-bytes=R,R' "$SRC/egl.c" ||
  fail "Merchant runtime receipt does not bind TexStorage ownership and bytes"

primary_compiler=${CC:-gcc}
command -v "$primary_compiler" >/dev/null 2>&1 ||
  fail "compiler not found: $primary_compiler"
compilers=("$primary_compiler")
if command -v clang >/dev/null 2>&1 &&
   [ "${primary_compiler##*/}" != clang ]; then
  compilers+=(clang)
fi

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes -Wconversion -Wsign-conversion -Wcast-qual)
for compiler in "${compilers[@]}"; do
  compiler_tag=${compiler##*/}
  "$compiler" "${STRICT[@]}" -O1 -I "$SRC" \
    -o "$WORK/merchant-single-channel-$compiler_tag" \
    "$HERE/test-single-channel-integration.c" \
    "$SRC/nxgl_single_channel.c" \
    "$SRC/nxgl_single_channel_adapter.c" -ldl ||
    fail "strict compile with $compiler"
  "$WORK/merchant-single-channel-$compiler_tag" ||
    fail "runtime contract with $compiler"
done

echo "merchant_single_channel_gate=PASS source_lock=PASS es2_la_rr=PASS es3_r8_red_swizzle=PASS legacy_255r=PASS rgba_passthrough=PASS texstorage_owner=backend compilers=${#compilers[@]}"
