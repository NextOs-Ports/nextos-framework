#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-CONTROLLERS-03 / C5B -- the whole Godot battery, once, in order.
#
# WHAT EACH GATE MAY CLAIM, AND NOTHING MORE
#   SOURCE_AUDIT     c5b_provenance_gate        source -> licence -> patch ->
#                                               binary, enforced not recorded
#   SOURCE_AUDIT     c5b_reconstruct_pristine   upstream recovered from the
#                                               seam patch and hash-pinned
#   SOURCE_AUDIT     godot_domain_gate          the ordinal domains, against
#                                               the pinned upstream sources
#   FIXTURE_HOST     test_godot_mapping         the pure adapter decisions
#   FIXTURE_HOST     test_godot_parser_negatives  the closed binding grammar
#   FIXTURE_HOST     test_godot_seam            the seam's fail-closed ladder:
#                                               the same function the two real
#                                               binaries have linked in
#   FIXTURE_HOST     godot_corpus_gate          official bytes against a
#                                               kernel-measured GUID oracle
#   REAL_API_HOST    godot_c5b_matrix_gate      what two REAL engines answered
#                                               in their own process, with the
#                                               seam linked into the binary
#   PENDING_PHYSICAL                            everything a device would prove
#
# A uinput pad is host evidence and never physical proof; no gate here may
# say otherwise.
#
# Every gate_id runs EXACTLY ONCE. Nothing is re-run to collect output.
#
# Required in the environment:
#   NXC5B_GODOT3     the pinned Godot 3 binary with the seam linked in
#   NXC5B_GODOT4     the pinned Godot 4 binary with the seam linked in
#   NXC5B_G3_SRC     the Godot 3 checkout those binaries were built from
#   NXC5B_G4_SRC     the Godot 4 checkout
#   NXC5B_SDL2_SRC   the pinned SDL2 src/joystick/linux/SDL_sysjoystick.c
#   NXC5B_BUNDLE     the sealed NEXTOSCONTROLLERS bundle (C3)
#   NXC5B_WORK       a writable directory for this attempt's artefacts
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")

fail() {
  echo "run-godot-c5b-host FAILED: $1" >&2
  exit 1
}

for var in NXC5B_GODOT3 NXC5B_GODOT4 NXC5B_G3_SRC NXC5B_G4_SRC \
           NXC5B_SDL2_SRC NXC5B_BUNDLE NXC5B_WORK; do
  eval "value=\${$var:-}"
  [ -n "$value" ] || fail "$var is not set"
done
[ -x "$NXC5B_GODOT3" ] || fail "NXC5B_GODOT3 is not executable"
[ -x "$NXC5B_GODOT4" ] || fail "NXC5B_GODOT4 is not executable"
[ -f "$NXC5B_BUNDLE" ] || fail "NXC5B_BUNDLE is not a file"
[ -f "$NXC5B_SDL2_SRC" ] || fail "NXC5B_SDL2_SRC is not a file"
mkdir -p "$NXC5B_WORK"
W=$NXC5B_WORK

CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=c99 -O1 -Wall -Wextra -Werror}

echo "== build the host probes  (CC=$CC CFLAGS=$CFLAGS)"
$CC $CFLAGS -I"$ROOT/include" -o "$W/test_godot_mapping" \
  "$HERE/test_godot_mapping.c" "$ROOT/src/nxinput_godot.c"
$CC $CFLAGS -I"$ROOT/include" -o "$W/test_godot_parser_negatives" \
  "$HERE/test_godot_parser_negatives.c" "$ROOT/src/nxinput_godot.c"
$CC $CFLAGS -I"$ROOT/include" -o "$W/test_godot_seam" \
  "$HERE/test_godot_seam.c" "$ROOT/src/nxinput_godot_seam.c" "$ROOT/src/nxinput_translate.c" "$ROOT/src/nxinput_sdl.c" \
  "$ROOT/src/nxinput_godot.c"
$CC $CFLAGS -I"$ROOT/include" -o "$W/godot_corpus_probe" \
  "$HERE/godot_corpus_probe.c" "$ROOT/src/nxinput_godot.c"
$CC $CFLAGS -I"$ROOT/include" -o "$W/godot_domain_probe" \
  "$HERE/godot_domain_probe.c" "$ROOT/src/nxinput_godot.c"
$CC $CFLAGS -I"$ROOT/include" -o "$W/c5b_v2_decide" \
  "$HERE/c5b_v2_decide.c" "$ROOT/src/nxinput_gptk.c" \
  "$ROOT/src/nxinput_gptk_motion.c" -lm

echo "== gate_id=provenance  class=SOURCE_AUDIT"
python3 -B "$HERE/c5b_provenance_gate.py" \
  --provenance "$ROOT/engine-patches/C5B-ENGINE-PROVENANCE.json" \
  --godot3 "$NXC5B_GODOT3" --godot4 "$NXC5B_GODOT4" \
  --godot3-src "$NXC5B_G3_SRC" --godot4-src "$NXC5B_G4_SRC" \
  || fail "the provenance chain does not hold"

echo "== gate_id=pristine  class=SOURCE_AUDIT"
python3 -B "$HERE/c5b_reconstruct_pristine.py" \
  --patches "$ROOT/engine-patches" \
  --godot3-tree "$NXC5B_G3_SRC" --godot4-tree "$NXC5B_G4_SRC" \
  --pins "$HERE/godot-source-pins.json" --out "$W/pristine" \
  || fail "the pristine upstream sources could not be recovered"

echo "== gate_id=domains  class=SOURCE_AUDIT"
python3 -B "$HERE/godot_domain_gate.py" --probe "$W/godot_domain_probe" \
  --godot3-source "$W/pristine/godot3-joypad_linux.cpp" \
  --godot4-source "$W/pristine/godot4-joypad_linux.cpp" \
  --sdl2-source "$NXC5B_SDL2_SRC" \
  --binary "godot3=$NXC5B_GODOT3" --binary "godot4=$NXC5B_GODOT4" \
  --pins "$HERE/godot-source-pins.json" \
  || fail "the ordinal domains no longer match the pinned sources"

echo "== gate_id=mapping_pure  class=FIXTURE_HOST"
"$W/test_godot_mapping" || fail "the pure adapter decisions regressed"

echo "== gate_id=parser_negatives  class=FIXTURE_HOST"
"$W/test_godot_parser_negatives" || fail "the binding grammar is not closed"

echo "== gate_id=seam_fail_closed  class=FIXTURE_HOST"
"$W/test_godot_seam" || fail "the seam's fail-closed ladder regressed"

echo "== gate_id=corpus  class=FIXTURE_HOST"
python3 -B "$HERE/godot_corpus_gate.py" --probe "$W/godot_corpus_probe" \
  --bundle "$NXC5B_BUNDLE" --corpus "$HERE/corpus/guid-capabilities.json" \
  --records-out "$W/corpus-records.txt" \
  || fail "the positive control failed"

echo "== gate_id=v2_chain  class=FIXTURE_HOST"
MAPPING=$W/mapping.txt
cat > "$MAPPING" <<'MAP'
0300000009120000a1c5000010010000,NXC5 Test Pad,a:b1,b:b0,x:b3,y:b2,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,platform:Linux,
MAP
RECEIPT=$(sha256sum "$NXC5B_BUNDLE" | cut -d' ' -f1)
for case in base ownerswap null; do
  python3 -B "$HERE/c5b_v2_to_mapping.py" --decider "$W/c5b_v2_decide" \
    --base-v2 "$HERE/corpus/c5b-v2-base.gptk" \
    --owner-v2 "$HERE/corpus/c5b-v2-$case.gptk" \
    --mapping "$MAPPING" --guid 0300000009120000a1c5000010010000 \
    --receipt "$RECEIPT" \
    --out-declaration "$W/decl-$case.txt" \
    --out-mapping "$W/mapping-$case.txt" \
    || fail "the V2 chain refused the $case configuration"
done

echo "== gate_id=matrix  class=REAL_API_HOST  (two real engines)"
# NOTE: paths here contain spaces. Nothing below may expand a path unquoted:
# the previous attempt failed exactly this way, splitting a binary's path on
# a space and reporting the run as green afterwards.
# The result list is accumulated straight into "$@": a shell string joined by
# spaces would split again on the first path that contains one.
set --
for which in godot3 godot4; do
  case $which in
    godot3) binary=$NXC5B_GODOT3; src=$NXC5B_G3_SRC; project=c5b-project-g3 ;;
    godot4) binary=$NXC5B_GODOT4; src=$NXC5B_G4_SRC; project=c5b-project-g4 ;;
  esac
  for scenario in matrix ownerswap null native twopads hotplug \
                    sigterm negatives; do
    case $scenario in
      ownerswap) decl=$W/decl-ownerswap.txt; mapping=$W/mapping-ownerswap.txt ;;
      null)      decl=$W/decl-null.txt;      mapping=$W/mapping-null.txt ;;
      native)    decl="";                    mapping=$W/mapping-base.txt ;;
      negatives) decl=$W/decl-base.txt;    mapping=$W/mapping-base.txt ;;
      *)         decl=$W/decl-base.txt;      mapping=$W/mapping-base.txt ;;
    esac
    out=$W/$which-$scenario.json
    echo "-- scenario $which/$scenario"
    if [ -n "$decl" ]; then
      python3 -B "$HERE/c5b_matrix_driver.py" \
        --engine "$binary" --which "$which" --checkout "$src" \
        --project "$HERE/$project" --scenario "$scenario" \
        --declaration "$decl" --mapping "$mapping" \
        --receipt "$W/receipt-$which-$scenario.txt" \
        --save "$W/save-$which-$scenario.txt" --out "$out" --seconds 400 \
        || fail "$which/$scenario did not complete"
    else
      python3 -B "$HERE/c5b_matrix_driver.py" \
        --engine "$binary" --which "$which" --checkout "$src" \
        --project "$HERE/$project" --scenario "$scenario" \
        --mapping "$mapping" \
        --receipt "$W/receipt-$which-$scenario.txt" \
        --save "$W/save-$which-$scenario.txt" --out "$out" --seconds 400 \
        || fail "$which/$scenario did not complete"
    fi
    set -- "$@" --result "$out"
  done
done

python3 -B "$HERE/godot_c5b_matrix_gate.py" "$@" \
  || fail "the functional matrix failed"

echo "run-godot-c5b-host: ALL GATES PASS"
echo "run-godot-c5b-host: physical validation remains PENDING_PHYSICAL"
