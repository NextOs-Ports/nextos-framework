#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-CONTROLLERS-03 / C6 -- the whole SDL battery, once, in order.
#
# WHAT EACH GATE MAY CLAIM, AND NOTHING MORE
#   SOURCE_AUDIT     c6_provenance_gate   source -> licence -> patch -> binary,
#                                          ENFORCED: the patch must reverse to
#                                          the pinned upstream bytes
#   SOURCE_AUDIT     c6_domain_gate        the ordinal domains, against the
#                                          four pinned SDL sources
#   FIXTURE_HOST     test_sdl_seam         the seam's fail-closed ladder: the
#                                          same function the three real SDL
#                                          libraries have linked in
#   FIXTURE_HOST     c6_corpus_gate        all 946 sealed C1 artifacts parsed
#                                          and classed; the E2E classes are
#                                          REAL_API_HOST
#   REAL_API_HOST    c6_matrix_gate        what THREE REAL SDL libraries
#                                          answered in their own processes,
#                                          with the seam linked in, over real
#                                          kernel pads
#   PENDING_PHYSICAL                       everything a device would prove
#
# A uinput pad is a real kernel device and real host evidence. It is NEVER
# physical proof: it shares this host's kernel and input stack, not a
# handheld's. No gate here may say otherwise.
#
# Every gate_id runs EXACTLY ONCE. Nothing is re-run to collect output.
#
# Required in the environment:
#   NXC6_SDL2_MIN     the built SDL2 2.28.5 prefix (seam linked in)
#   NXC6_SDL2_CUR     the built SDL2 2.32.10 prefix
#   NXC6_SDL3         the built SDL3 3.2.30 prefix
#   NXC6_TREES        the directory holding the three built checkouts
#   NXC6_PRISTINE     the directory holding the four pinned pristine sources
#   NXC6_CORPUS       the sealed C1 controls-corpus-v1.json
#   NXC6_PM_GIT       the pinned PortMaster-New store (.git)
#   NXC6_WORK         a writable directory for this attempt's artefacts
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")

fail() {
  echo "run-sdl-c6-host FAILED: $1" >&2
  exit 1
}

for var in NXC6_SDL2_MIN NXC6_SDL2_CUR NXC6_SDL3 NXC6_TREES NXC6_PRISTINE \
           NXC6_CORPUS NXC6_PM_GIT NXC6_WORK; do
  eval "value=\${$var:-}"
  [ -n "$value" ] || fail "$var is not set"
done
[ -f "$NXC6_CORPUS" ] || fail "NXC6_CORPUS is not a file"
[ -d "$NXC6_PM_GIT" ] || fail "NXC6_PM_GIT is not a directory"
for fixture in sdl2-2.0.10.c sdl2-2.28.5.c sdl2-2.32.10.c \
               sdl3-3.2.30.c init/sdl2-2.28.5-SDL_joystick.c \
               init/sdl2-2.32.10-SDL_joystick.c init/sdl3-3.2.30-SDL.c; do
  [ -f "$NXC6_PRISTINE/$fixture" ] || \
    fail "NXC6_PRISTINE is incomplete: missing $fixture"
done
mkdir -p "$NXC6_WORK"
W=$NXC6_WORK

CORPUS_SHA=c5a2da52a428859dee97441aa174451d4554293ad159047991ecd42d6143cb68
CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=c99 -O1 -Wall -Wextra -Werror}
SEAM_SRC="$ROOT/src/nxinput_sdl_seam.c $ROOT/src/nxinput_sdl.c \
$ROOT/src/nxinput_portmaster.c \
$ROOT/src/nxinput_sovereign.c \
$ROOT/src/nxinput_authority.c \
$ROOT/src/nxinput_livedb.c \
$ROOT/src/nxinput_decision.c $ROOT/src/nxinput_provider.c $ROOT/src/nxinput_godot.c"

echo "== build the host probes  (CC=$CC CFLAGS=$CFLAGS)"
# shellcheck disable=SC2086
$CC $CFLAGS -I"$ROOT/include" -o "$W/test_sdl_seam" \
  "$HERE/test_sdl_seam.c" $SEAM_SRC
# shellcheck disable=SC2086
$CC $CFLAGS -I"$ROOT/include" -o "$W/test_portmaster_domain" \
  "$HERE/test_portmaster_domain.c" "$ROOT/src/nxinput_portmaster.c" \
  "$ROOT/src/nxinput_sdl.c"
# shellcheck disable=SC2086
$CC $CFLAGS -I"$ROOT/include" -o "$W/c6_domain_probe" \
  "$HERE/c6_domain_probe.c" "$ROOT/src/nxinput_sdl.c" \
  "$ROOT/src/nxinput_godot.c"

echo "== build the REAL consumers, one per pinned SDL"
# NOTE: every path below is quoted. The C5B attempt that split a path on a
# space and still reported green is the reason this is called out here.
build_consumer() {
  prefix=$1; out=$2; extra=$3; inc=$4
  # shellcheck disable=SC2086
  $CC -std=gnu99 -O1 -Wall -Wextra $extra -I"$ROOT/include" -I"$inc" \
    -o "$out" "$HERE/c6_consumer.c" $SEAM_SRC \
    -L"$prefix/lib" $5 -Wl,-rpath,"$prefix/lib" || fail "consumer $out"
}
build_consumer "$NXC6_SDL2_MIN" "$W/c6_consumer_sdl2-2.28.5" "" \
  "$NXC6_SDL2_MIN/include/SDL2" -lSDL2
build_consumer "$NXC6_SDL2_CUR" "$W/c6_consumer_sdl2-2.32.10" "" \
  "$NXC6_SDL2_CUR/include/SDL2" -lSDL2
build_consumer "$NXC6_SDL3" "$W/c6_consumer_sdl3-3.2.30" "-DC6_SDL3" \
  "$NXC6_SDL3/include" -lSDL3

echo "== gate_id=provenance  class=SOURCE_AUDIT"
python3 -B "$HERE/c6_provenance_gate.py" \
  --provenance "$ROOT/engine-patches/C6-SDL-PROVENANCE.json" \
  --pins "$HERE/sdl-source-pins.json" --framework "$ROOT" \
  --out "$W/pristine" \
  --tree "sdl2-2.28.5=$NXC6_TREES/sdl2-2.28.5" \
  --tree "sdl2-2.32.10=$NXC6_TREES/sdl2-2.32.10" \
  --tree "sdl3-3.2.30=$NXC6_TREES/sdl3-3.2.30" \
  --binary "sdl2-2.28.5=$NXC6_SDL2_MIN/lib/libSDL2-2.0.so.0.2800.5" \
  --binary "sdl2-2.32.10=$NXC6_SDL2_CUR/lib/libSDL2-2.0.so.0.3200.10" \
  --binary "sdl3-3.2.30=$NXC6_SDL3/lib/libSDL3.so.0.2.30" \
  || fail "the provenance chain does not hold"

echo "== gate_id=domains  class=SOURCE_AUDIT"
python3 -B "$HERE/c6_domain_gate.py" --probe "$W/c6_domain_probe" \
  --pins "$HERE/sdl-source-pins.json" \
  --source "sdl2-2.0.10=$NXC6_PRISTINE/sdl2-2.0.10.c" \
  --source "sdl2-2.28.5=$NXC6_PRISTINE/sdl2-2.28.5.c" \
  --source "sdl2-2.32.10=$NXC6_PRISTINE/sdl2-2.32.10.c" \
  --source "sdl3-3.2.30=$NXC6_PRISTINE/sdl3-3.2.30.c" \
  --init-source "sdl2-2.28.5=$NXC6_PRISTINE/init/sdl2-2.28.5-SDL_joystick.c" \
  --init-source "sdl2-2.32.10=$NXC6_PRISTINE/init/sdl2-2.32.10-SDL_joystick.c" \
  --init-source "sdl3-3.2.30=$NXC6_PRISTINE/init/sdl3-3.2.30-SDL.c" \
  || fail "the ordinal domains or the mapping-init order no longer match the \
pinned sources"

echo "== gate_id=seam_fail_closed  class=FIXTURE_HOST"
"$W/test_sdl_seam" || fail "the seam's fail-closed ladder regressed"

echo "== gate_id=portmaster_domain  class=FIXTURE_HOST"
"$W/test_portmaster_domain" || \
  fail "the capability-proved PortMaster domain projection regressed"

echo "== gate_id=matrix  class=REAL_API_HOST  (three real SDL libraries)"
# The result list accumulates straight into "$@": a shell string joined by
# spaces would split again on the first path that contains one.
set --
for which in sdl2-2.28.5 sdl2-2.32.10 sdl3-3.2.30; do
  consumer=$W/c6_consumer_$which
  for scenario in matrix null ownerswap crc_alias native priority cfw_db bundle \
                  builtin raw_declared chord twopads guid_same_mapping \
                  guid_divergent hotplug muos_joydev muos_rom_exact \
                  muos_rom_bundle \
                  stickless_combined stickless_plain keyboard_only \
                  negative_syntax negative_empty \
                  negative_other_guid corpus1 corpus2 corpus3; do
    out=$W/$which-$scenario.json
    echo "-- scenario $which/$scenario"
    nice -n 10 python3 -B "$HERE/c6_matrix_driver.py" \
      --consumer "$consumer" --which "$which" --scenario "$scenario" \
      --work "$W/runs" --out "$out" --seconds 16 \
      --corpus "$NXC6_CORPUS" --corpus-git-dir "$NXC6_PM_GIT" \
      || fail "$which/$scenario did not complete"
    set -- "$@" --result "$out"
  done
done

python3 -B "$HERE/c6_matrix_gate.py" "$@" \
  || fail "the functional matrix failed"

echo "== gate_id=muos_layout  class=REAL_API_HOST  (0.10.0 layout authority)"
python3 -B "$HERE/muos_layout_gate.py" \
  --consumer "sdl2-2.28.5=$W/c6_consumer_sdl2-2.28.5" \
  --consumer "sdl2-2.32.10=$W/c6_consumer_sdl2-2.32.10" \
  --consumer "sdl3-3.2.30=$W/c6_consumer_sdl3-3.2.30" \
  --work "$W/runs" \
  || fail "the muOS layout authority failed"

echo "== gate_id=corpus  class=FIXTURE_HOST"
python3 -B "$HERE/c6_corpus_gate.py" --corpus "$NXC6_CORPUS" \
  --corpus-sha256 "$CORPUS_SHA" --git-dir "$NXC6_PM_GIT" \
  --out "$W/c6-corpus-classes.json" "$@" \
  || fail "the corpus classes failed"

echo "run-sdl-c6-host: ALL GATES PASS"
echo "run-sdl-c6-host: physical validation remains PENDING_PHYSICAL"
