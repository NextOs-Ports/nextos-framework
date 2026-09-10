#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# nxinput 0.11.0 -- V5 host gate: every pure V5 test compiled directly (same
# source lists as CMakeLists.txt), the provider-pin audit and the mutation
# runner over the C machines. No device, no SDL header in the pure modules.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NX=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-v5.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail() { echo "nxinput_v5=FAIL $*" >&2; exit 1; }
FLAGS=(-std=c99 -Wall -Wextra -Werror -Wno-format-truncation -Wno-misleading-indentation -D_POSIX_C_SOURCE=200809L -I"$NX/include" -I"$NX/engine-glue")
S="$NX/src"; V="$HERE/v5"
build() { # name sources...
  local name=$1; shift
  "$CC" "${FLAGS[@]}" -o "$WORK/test_v5_$name" "$V/test_v5_$name.c" "$@" -ldl -lm -lpthread || fail "test_v5_$name does not compile"
}
build incident_red "$S/nxinput_portmaster.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build provider "$S/nxinput_provider.c" "$S/nxinput_provider_linux.c" "$S/nxinput_sha256.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build translate "$S/nxinput_translate.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build padset "$NX/engine-glue/nxinput_padset.c" "$S/nxinput_prerouter.c" "$S/nxinput_axis_calib.c"
build axis_calib "$S/nxinput_axis_calib.c"
build gptk4 "$S/nxinput_gptk4.c"
build route "$S/nxinput_route.c"
build registry "$S/nxinput_registry.c" "$S/nxinput_gptk4.c"
build seam "$S/nxinput_sdl_seam.c" "$S/nxinput_decision.c" "$S/nxinput_provider.c" "$S/nxinput_sdl.c" "$S/nxinput_portmaster.c" "$S/nxinput_sovereign.c" "$S/nxinput_authority.c" "$S/nxinput_livedb.c" "$S/nxinput_godot.c"
build decision "$S/nxinput_decision.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build corpus "$S/nxinput_corpus.c"
build prerouter "$S/nxinput_prerouter.c" "$S/nxinput_decision.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build lifecycle "$S/nxinput_lifecycle.c"
build gptk4_bridge "$S/nxinput_gptk4_bridge.c" "$S/nxinput_gptk4.c" "$S/nxinput_gptk.c" "$S/nxinput_gptk_motion.c" "$S/nxinput_sha256.c"
build gptk_live_vector "$S/nxinput_gptk_live.c" "$S/nxinput_gptk4_bridge.c" "$S/nxinput_gptk4.c" "$S/nxinput_gptk.c" "$S/nxinput_gptk_motion.c" "$S/nxinput_sha256.c"
build authority_v5 "$S/nxinput_authority_v5.c" "$S/nxinput_decision.c" "$S/nxinput_gptk4.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build keyboard "$S/nxinput_keyboard.c" "$S/nxinput_route.c" "$S/nxinput_registry.c" "$S/nxinput_gptk4.c"
build route_policy "$S/nxinput_route_policy.c" "$S/nxinput_decision.c" "$S/nxinput_provider.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c"
build godot_seam_ascending "$S/nxinput_godot_seam.c" "$S/nxinput_godot.c" "$S/nxinput_translate.c" "$S/nxinput_sdl.c" "$S/nxinput_sha256.c"
build coexist "$S/nxinput_coexist.c" "$S/nxinput_provider.c" "$S/nxinput_provider_linux.c" "$S/nxinput_sha256.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c" "$S/nxinput_prerouter.c"
build gptk4_preinit "$S/nxinput_gptk4_preinit.c" "$S/nxinput_gptk4_bridge.c" "$S/nxinput_gptk4.c" "$S/nxinput_gptk.c" "$S/nxinput_gptk_motion.c" "$S/nxinput_sha256.c" "$S/nxinput_gptk_loader.c" "$S/nxinput_gptk_preinit.c"
count=0
for t in incident_red provider translate padset axis_calib route seam decision corpus prerouter lifecycle route_policy godot_seam_ascending; do
  "$WORK/test_v5_$t" > "$WORK/$t.log" 2>&1 || { tail -5 "$WORK/$t.log" >&2; fail "test_v5_$t"; }
  count=$((count + 1))
done
for t in gptk4 registry gptk4_bridge gptk4_preinit gptk_live_vector authority_v5 keyboard; do
  "$WORK/test_v5_$t" "$V/corpus/fp2-complete.gptk" > "$WORK/$t.log" 2>&1 || { tail -5 "$WORK/$t.log" >&2; fail "test_v5_$t"; }
  count=$((count + 1))
done
# B8: real DSOs when present (77 = skip, counted separately). On a host whose
# SDL2 is sdl2-compat the case proved is SHARED CORE; with
# NXINPUT_CLASSIC_SDL2_DIR pointing at a classic libSDL2-2.0.so.0 (built from
# the pinned 2.32.10 tarball, see tests/providers/classic-sdl2-2.32.10.txt)
# the SEPARATE case (real isolation) runs too.
if "$WORK/test_v5_coexist" > "$WORK/coexist.log" 2>&1; then count=$((count + 1)); elif [ $? -eq 77 ]; then echo "coexist: SKIP (no SDL2+SDL3 on this host)"; else tail -5 "$WORK/coexist.log" >&2; fail "test_v5_coexist"; fi
if [ -n "${NXINPUT_CLASSIC_SDL2_DIR:-}" ] && [ -f "$NXINPUT_CLASSIC_SDL2_DIR/libSDL2-2.0.so.0" ]; then
  LD_LIBRARY_PATH="$NXINPUT_CLASSIC_SDL2_DIR" "$WORK/test_v5_coexist" > "$WORK/coexist-classic.log" 2>&1 || { tail -5 "$WORK/coexist-classic.log" >&2; fail "test_v5_coexist (classic SDL2: separate providers)"; }
  grep -q "arbitration=separate" "$WORK/coexist-classic.log" || fail "classic SDL2 did not arbitrate as separate"
  count=$((count + 1))
fi
# I3a: sanitizer fuzz over lifecycle / pre-router / router / keyboard (invariants, 200 runs)
"$CC" -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -I"$NX/include" -o "$WORK/fuzz_v5_lifecycle" "$V/fuzz_v5_lifecycle.c" "$S/nxinput_lifecycle.c" "$S/nxinput_prerouter.c" "$S/nxinput_route.c" "$S/nxinput_keyboard.c" "$S/nxinput_gptk4.c" || fail "fuzz_v5_lifecycle does not compile under sanitizers"
ASAN_OPTIONS=detect_leaks=1 "$WORK/fuzz_v5_lifecycle" "$V/corpus/fp2-complete.gptk" 200 > "$WORK/fuzz.log" 2>&1 || { tail -5 "$WORK/fuzz.log" >&2; fail "fuzz_v5_lifecycle"; }
count=$((count + 1))
# review finding 1 RED case on the host: an UNPINNED provider must be stock, never mute (needs /dev/uinput)
if [ -w /dev/uinput ] && [ "${NXINPUT_V5_SKIP_UINPUT:-0}" != 1 ]; then
  bash "$V/run-unpinned-provider-oracle.sh" "$WORK/unpinned" > "$WORK/unpinned.log" 2>&1 || { tail -12 "$WORK/unpinned.log" >&2; fail "unpinned provider oracle (stock mode)"; }
  count=$((count + 1))
else
  echo "unpinned provider oracle: SKIP (no writable /dev/uinput)"
fi
# H5/H8: the complete chain judged by the independent oracle (8 mutants)
"$CC" "${FLAGS[@]}" -o "$WORK/chain_harness" "$V/chain_harness.c" "$S/nxinput_sdl.c" "$S/nxinput_godot.c" "$S/nxinput_translate.c" "$S/nxinput_gptk4.c" "$S/nxinput_route.c" "$S/nxinput_registry.c" "$S/nxinput_keyboard.c" "$S/nxinput_axis_calib.c" -lm || fail "chain_harness does not compile"
python3 "$V/test_v5_chain.py" "$WORK/chain_harness" "$NX" > "$WORK/chain.log" 2>&1 || { tail -8 "$WORK/chain.log" >&2; fail "chain oracle"; }
count=$((count + 1))
# M1c: the universal schema-4 host gate over the Tearscape owner fixture (RED test)
"$CC" "${FLAGS[@]}" -o "$WORK/nx-gptk4-host-gate" "$NX/tools/nx-gptk4-host-gate.c" "$S/nxinput_gptk4_preinit.c" "$S/nxinput_gptk4_bridge.c" "$S/nxinput_gptk4.c" "$S/nxinput_gptk.c" "$S/nxinput_gptk_motion.c" "$S/nxinput_gptk_live.c" "$S/nxinput_sha256.c" "$S/nxinput_gptk_loader.c" "$S/nxinput_gptk_preinit.c" -lm || fail "nx-gptk4-host-gate does not compile"
python3 "$V/test_v5_gptk4_host_gate.py" "$WORK/nx-gptk4-host-gate" "$NX" > "$WORK/hostgate.log" 2>&1 || { tail -8 "$WORK/hostgate.log" >&2; fail "gptk4 host gate"; }
count=$((count + 1))
"$CC" "${FLAGS[@]}" -o "$WORK/nx-gptk4-check" "$NX/tools/nx-gptk4-check.c" "$S/nxinput_gptk4_bridge.c" "$S/nxinput_gptk4.c" "$S/nxinput_gptk.c" "$S/nxinput_gptk_motion.c" "$S/nxinput_sha256.c" -lm || fail "nx-gptk4-check does not compile"
"$WORK/nx-gptk4-check" "$V/corpus/fp2-complete.gptk" --contexts menu,pause,cursor --keyboard fp2.primary fp2.secondary fp2.special fp2.guard fp2.pause fp2.move:vector fp2.brake:axis fp2.aim:vector menu.confirm menu.cancel player.jump > "$WORK/check1.log" || { cat "$WORK/check1.log" >&2; fail "canonical corpus rejected by the checker"; }
"$WORK/nx-gptk4-check" "$V/corpus/fp2-generated-v4.gptk" --contexts menu fp2.attack fp2.cancel fp2.confirm fp2.guard fp2.jump fp2.move:vector fp2.pause fp2.special > "$WORK/check2.log" || { cat "$WORK/check2.log" >&2; fail "generated FP2 default rejected by the checker"; }
# 0.11.2: no direct call to a post-floor SDL entry point in the vendored runtime (mutant inside)
python3 "$V/test_v5_sdl_floor.py" "$NX" > "$WORK/sdlfloor.log" 2>&1 || { tail -5 "$WORK/sdlfloor.log" >&2; fail "sdl floor gate"; }
count=$((count + 1))
# 0.11.3: the staging "too late" guard keys on the joystick subsystem, not any (mutant inside)
python3 "$V/test_v5_stage_subsystem.py" "$NX" > "$WORK/stagesub.log" 2>&1 || { tail -5 "$WORK/stagesub.log" >&2; fail "stage subsystem gate"; }
count=$((count + 1))
python3 "$V/test_v5_provider_pins.py" > "$WORK/pins.log" 2>&1 || { tail -5 "$WORK/pins.log" >&2; fail "provider pins"; }
NXINPUT_V5_BIN="$WORK" python3 "$V/mutation/run_mutants.py" > "$WORK/mutants.log" 2>&1 || { tail -8 "$WORK/mutants.log" >&2; fail "mutants"; }
killed=$(grep -c " killed " "$WORK/mutants.log" || true)
survivors=$(grep -E "^V5 mutants surviving: " "$WORK/mutants.log" | awk '{print $NF}')
[ "$survivors" = 0 ] || fail "V5 mutants surviving: $survivors"
bash "$HERE/static_no_device_name_fallback.sh" "$NX" > "$WORK/static.log" 2>&1 || { cat "$WORK/static.log" >&2; fail "static device-name gate"; }
echo "nxinput_v5 gate: PASS tests=$count pins=1 mutants_killed=$killed survivors=0 static_gate=clean"
