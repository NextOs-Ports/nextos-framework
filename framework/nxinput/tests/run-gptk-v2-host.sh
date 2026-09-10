#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-CONTROLLERS-03 / C4 -- NEXTOSCONTROLLERS v2.
#
# 1. test_gptk_v2 (tri-state, completeness, the owner's acceptance case in a
#    hermetic consumer rig) and test_gptk_upgrade (the owner's file is never
#    overwritten) under GCC, Clang, ASAN and UBSAN.
# 2. Static audit of the LIFECYCLE BOUNDARY: the SELECT+START chord must not
#    read the mapping, and L2/R2 can never enter it.
# 3. The sealed C1 corpus: no gptokeyb artifact and no upstream token may be
#    accepted as a NextOS map. Exits 77 (visible SKIP) without the corpus.
#
# CLAIM BOUNDARY: everything here is CORE_HERMETIC. No SDL, Godot, Android,
# Unity, touch or device claim is made or implied -- those are missions
# 116-119.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXINPUT=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-gptk-v2.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "nxinput_gptk_v2=FAIL $*" >&2; exit 1; }

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes -D_POSIX_C_SOURCE=200809L
        "-DNXINPUT_V2_FIXTURE=\"$HERE/corpus/ok-v2-generated.gptk\"")
CORE=("$NXINPUT/src/nxinput_gptk.c" "$NXINPUT/src/nxinput_gptk_motion.c")

compilers=("$CC")
if command -v clang >/dev/null 2>&1 && [ "$CC" != clang ]; then
  compilers+=(clang)
fi

for compiler in "${compilers[@]}"; do
  for san in "" "-fsanitize=address -fno-omit-frame-pointer" \
             "-fsanitize=undefined -fno-omit-frame-pointer"; do
    tag="$compiler${san:+-${san#-fsanitize=}}"; tag=${tag%% *}
    # shellcheck disable=SC2086
    "$compiler" "${STRICT[@]}" $san -I "$NXINPUT/include" \
      -o "$WORK/v2-$tag" "$HERE/test_gptk_v2.c" "${CORE[@]}" \
      "$NXINPUT/src/nxinput_exit_chord.c" -lm ||
      fail "test_gptk_v2 did not compile cleanly ($compiler $san)"
    "$WORK/v2-$tag" || fail "test_gptk_v2 ($compiler $san)"
    # shellcheck disable=SC2086
    "$compiler" "${STRICT[@]}" $san -I "$NXINPUT/include" \
      -o "$WORK/upgrade-$tag" "$HERE/test_gptk_upgrade.c" \
      "$NXINPUT/src/nxinput_gptk_upgrade.c" "${CORE[@]}" -lm ||
      fail "test_gptk_upgrade did not compile cleanly ($compiler $san)"
    "$WORK/upgrade-$tag" || fail "test_gptk_upgrade ($compiler $san)"
  done
done

# ------------------------------------------- lifecycle boundary (static)
# The SELECT+START chord is out-of-band: it must not consult the mapping, the
# dispatcher or the decision, and L2/R2 must be unreachable from it. `null` on
# SELECT/START therefore cannot disarm it -- which the runtime gate also
# proves by firing the chord from a file where both are null.
CHORD="$NXINPUT/src/nxinput_exit_chord.c"
! grep -Eq 'nxinput_gptk_action|nxinput_gptk_decide|nxinput_gptk_dispatcher|nxinput_gptk_parse' \
    "$CHORD" ||
  fail "the exit chord reads the mapping; it must stay out-of-band"
! grep -Eq 'NXINPUT_GPTK_L2|NXINPUT_GPTK_R2|lefttrigger|righttrigger' "$CHORD" ||
  fail "the exit chord references a trigger; L2/R2 can never enter it"
[ "$(grep -c 'NXINPUT_GPTK_SELECT' "$CHORD")" -ge 1 ] ||
  fail "the exit chord no longer names SELECT"
[ "$(grep -c 'NXINPUT_GPTK_START' "$CHORD")" -ge 1 ] ||
  fail "the exit chord no longer names START"
# The chord takes normalized SELECT/START state as ARGUMENTS; there is no
# remap hook, no substitute button and no GUID/layout in its source.
! grep -Eq 'guid|GUID|layout|fallback_button|substitute' "$CHORD" ||
  fail "the exit chord grew a remap/GUID/substitute path"

# The tri-state values are recognized through the published macros, and a
# misspelled disable has its own explicit refusal instead of falling through
# to the generic "invalid action name".
grep -q 'NXINPUT_GPTK_LITERAL_NULL' "$NXINPUT/src/nxinput_gptk.c" ||
  fail "the parser does not use NXINPUT_GPTK_LITERAL_NULL"
grep -q 'NXINPUT_GPTK_LITERAL_NATIVE' "$NXINPUT/src/nxinput_gptk.c" ||
  fail "the parser does not use NXINPUT_GPTK_LITERAL_NATIVE"
grep -q 'gptk_literal_lookalike' "$NXINPUT/src/nxinput_gptk.c" ||
  fail "a misspelled null/native no longer has its own refusal"

# -------------------------------------------------------------- corpus
if [ -z "${NX_CONTROLS_CORPUS_V1:-}" ] || [ ! -f "${NX_CONTROLS_CORPUS_V1:-}" ]
then
  echo "nxinput_gptk_v2=SKIP corpus not supplied (NX_CONTROLS_CORPUS_V1)" >&2
  exit 77
fi

"$CC" "${STRICT[@]}" -I "$NXINPUT/include" -o "$WORK/probe" \
  "$HERE/gptk_v2_probe.c" "${CORE[@]}" -lm ||
  fail "the V2 corpus probe did not compile cleanly"

python3 -B "$HERE/gptk_v2_corpus_gate.py" \
  --corpus "$NX_CONTROLS_CORPUS_V1" --probe "$WORK/probe" \
  --work "$WORK/corpus" ${NX_PM_GIT:+--git-dir "$NX_PM_GIT"} ||
  fail "the V2 corpus gate failed"

echo "nxinput_gptk_v2=PASS v2=1 upgrade=1 lifecycle_static=1 corpus=1 " \
     "claim=CORE_HERMETIC engine_adapters=PENDING_116_119"
