#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-CONTROLLERS-03 / C3 corpus gate.
#
# Requires the sealed C1 corpus (env NX_CONTROLS_CORPUS_V1 pointing at
# controls-corpus-v1.json). Exits 77 (external-artifact SKIP, a visible limit
# of claim) when the corpus is not supplied -- never a silent PASS.
#
# 1. test_sovereign under GCC, Clang, ASAN, UBSAN.
# 2. EVERY artifact of the sealed corpus goes through parsing,
#    classification and deduplication (the python orchestrator), and every
#    embedded gamecontrollerdb line goes through the C syntax validator.
# 3. EVERY GUID entry of EVERY gamecontrollerdb artifact runs the FULL
#    pipeline (authority resolution with measured caps + effective readback)
#    and is judged by an INDEPENDENT reference (tests/sovereign_reference.py),
#    which decides for itself which entry wins and with which bindings.
# 4. MUTANT PROOF: the driver is rebuilt from a deliberately mutated resolver
#    that changes one binding of the winning line. The gate MUST reject it --
#    otherwise the differential proof would be comparing code with itself.
# 5. Static audit: the sovereign core is pure and mentions no brand, the
#    production path owns no second mapping route.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXINPUT=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-sovereign.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "nxinput_sovereign=FAIL $*" >&2; exit 1; }

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes -Wconversion -Wsign-conversion -Wcast-qual
        -D_POSIX_C_SOURCE=200809L)

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
      -o "$WORK/sovereign-$tag" \
      "$HERE/test_sovereign.c" "$NXINPUT/src/nxinput_sovereign.c" ||
      fail "test_sovereign did not compile cleanly ($compiler $san)"
    "$WORK/sovereign-$tag" || fail "test_sovereign ($compiler $san)"
    # The PRODUCTION adapter (the path nxinput.c executes) under the same
    # compilers and sanitizers: hotplug, two pads with one GUID, hostile
    # setter, missing database, refusal before gameplay.
    # shellcheck disable=SC2086
    "$compiler" "${STRICT[@]}" $san -I "$NXINPUT/include" \
      -o "$WORK/authority-$tag" \
      "$HERE/test_authority.c" "$NXINPUT/src/nxinput_authority.c" \
      "$NXINPUT/src/nxinput_sovereign.c" ||
      fail "test_authority did not compile cleanly ($compiler $san)"
    "$WORK/authority-$tag" || fail "test_authority ($compiler $san)"
  done
done

# ------------------------------------------------------------ static audit
! grep -Eq '(^|[^A-Z_])SDL_|getenv|dlopen|dlsym|fopen|socket|system\(' \
    "$NXINPUT/src/nxinput_sovereign.c" ||
  fail "nxinput_sovereign.c is not pure"
for token in mali Mali panfrost rocknix ArkOS arkos muos NextOS RG40 R36 \
             rk3326 Adreno anbernic; do
  ! grep -Fq -- "$token" "$NXINPUT/src/nxinput_sovereign.c" ||
    fail "nxinput_sovereign.c mentions '$token'"
  ! grep -Fq -- "$token" "$NXINPUT/include/nxinput_sovereign.h" ||
    fail "nxinput_sovereign.h mentions '$token'"
done
# The production path has exactly ONE mapping route (mission 114A): nxinput.c
# applies no mapping itself and admits every pad through the sovereign
# adapter. A second, older path would show up as a direct SDL setter here.
[ "$(grep -c 'SDL_GameControllerAddMapping' "$NXINPUT/src/nxinput.c")" -eq 0 ] ||
  fail "nxinput.c applies a mapping outside the sovereign authority"
[ "$(grep -c 'nxinput_apply_inherited_mappings' "$NXINPUT/src/nxinput.c")" \
    -eq 0 ] ||
  fail "the blind inherited-mapping loader grew back"
grep -q 'nxinput_authority_admit(&input->authority' "$NXINPUT/src/nxinput.c" ||
  fail "nxinput.c does not admit pads through the sovereign authority"
grep -q 'nxinput_authority_forget(&input->authority' \
     "$NXINPUT/src/nxinput.c" ||
  fail "nxinput.c does not invalidate a disconnected pad's decision"
! grep -Eq '(^|[^A-Z_])SDL_|dlopen|dlsym|fopen|socket|system\(' \
    "$NXINPUT/src/nxinput_authority.c" ||
  fail "nxinput_authority.c is not free of direct effects"
# The SDL backing is effects-only: it must never compare a name or a GUID.
! grep -Eq 'strcmp|strstr|strncmp' "$NXINPUT/src/nxinput_authority_sdl.c" ||
  fail "nxinput_authority_sdl.c decides something instead of only acting"

# The default open path must not regrow a post-load rewrite: the removed
# routines exist only inside the explanatory comment, never as code.
[ "$(grep -c 'static .*nxinput_normalize_device_mapping' \
      "$NXINPUT/src/nxinput.c")" -eq 0 ] ||
  fail "nxinput_normalize_device_mapping grew back"
[ "$(grep -c 'nxinput_rewrite_portmaster_handheld_mapping(' \
      "$NXINPUT/src/nxinput.c")" -eq 0 ] ||
  fail "the handheld rewrite grew back"
[ "$(grep -c 'nxinput_has_portmaster_handheld_gap(' \
      "$NXINPUT/src/nxinput.c")" -eq 0 ] ||
  fail "the handheld gap detector grew back"

# -------------------------------------------------------------- the corpus
if [ -z "${NX_CONTROLS_CORPUS_V1:-}" ] || [ ! -f "${NX_CONTROLS_CORPUS_V1:-}" ]
then
  echo "nxinput_sovereign=SKIP corpus not supplied (NX_CONTROLS_CORPUS_V1)" >&2
  exit 77
fi

"$CC" "${STRICT[@]}" -I "$NXINPUT/include" -o "$WORK/driver" \
  "$HERE/sovereign_corpus_driver.c" "$NXINPUT/src/nxinput_sovereign.c" ||
  fail "the corpus driver did not compile cleanly"

mkdir -p "$WORK/real" "$WORK/mutant"

python3 -B "$HERE/sovereign_corpus_gate.py" \
  --corpus "$NX_CONTROLS_CORPUS_V1" --driver "$WORK/driver" \
  --work "$WORK/real" ${NX_PM_GIT:+--git-dir "$NX_PM_GIT"} ||
  fail "the corpus gate failed"

# ------------------------------------------------------------ mutant proof
# The differential proof is only worth something if it can DETECT a resolver
# that changes a binding. A copy of the resolver is mutated so that the
# candidate it applies has one button ordinal bumped, and the very same gate
# is required to REJECT it. A mutant that passes means the comparison is
# circular.
python3 - "$NXINPUT/src/nxinput_sovereign.c" "$WORK/mutant/mutant.c" <<'MUTATE'
import sys
source = open(sys.argv[1], encoding="utf-8").read()
anchor = """    nxinput_sovereign_decision *decision) {
  nxinput_sovereign_reason reason;
"""
assert anchor in source, "the mutation anchor moved; fix the mutant proof"
injected = anchor + """  /* MUTANT (test only, never in the repository): change ONE binding of the
   * candidate before it is applied. The gate must notice. */
  char nx_mutant_line[NXINPUT_SOVEREIGN_LINE_MAX];
  {
    char *nx_mutant_hit;
    (void)snprintf(nx_mutant_line, sizeof nx_mutant_line, "%s", line);
    nx_mutant_hit = strstr(nx_mutant_line, ":b");
    if (nx_mutant_hit != NULL && nx_mutant_hit[2] >= '1' &&
        nx_mutant_hit[2] <= '9') {
      /* LOWER the ordinal: the mutated binding stays inside the measured
       * capabilities, so only a real binding comparison can catch it. */
      nx_mutant_hit[2]--;
    }
    line = nx_mutant_line;
  }
"""
open(sys.argv[2], "w", encoding="utf-8").write(source.replace(anchor, injected, 1))
MUTATE

"$CC" -std=c99 -Wall -D_POSIX_C_SOURCE=200809L -I "$NXINPUT/include" \
  -o "$WORK/driver-mutant" "$HERE/sovereign_corpus_driver.c" \
  "$WORK/mutant/mutant.c" ||
  fail "the mutant driver did not compile"

if python3 -B "$HERE/sovereign_corpus_gate.py" \
     --corpus "$NX_CONTROLS_CORPUS_V1" --driver "$WORK/driver-mutant" \
     --work "$WORK/mutant" ${NX_PM_GIT:+--git-dir "$NX_PM_GIT"} \
     >"$WORK/mutant.log" 2>&1; then
  fail "the mutant resolver PASSED the differential proof (it is circular)"
fi
grep -q "sovereign_corpus_gate FAILED" "$WORK/mutant.log" ||
  fail "the mutant run failed for a reason other than the differential proof"
# The mutated binding stays reachable, so the rejection must come from the
# binding/byte comparison itself -- not from a capability side effect.
grep -Eq "BINDING DIVERGENCE|bytes the reference did not authorize" \
     "$WORK/mutant.log" ||
  fail "the mutant was rejected for the wrong reason (not a binding change)"
echo "mutant rejected: $(grep -m1 "sovereign_corpus_gate FAILED" \
  "$WORK/mutant.log" | cut -c1-160)"

echo "nxinput_sovereign=PASS unit=1 authority=1 static=1 corpus=1 mutant=1"
