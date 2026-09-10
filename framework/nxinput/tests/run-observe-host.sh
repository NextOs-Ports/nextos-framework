#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-CONTROLLERS-03 / C2 host gate for nxinput_observe + nxinput-doctor.
#
# 1. The pure observer module under GCC, Clang, ASAN and UBSAN: schemas,
#    goldens, redaction, all 18 controls, bounded events, hotplug/two pads,
#    chord negatives, pending consumer, ON/OFF replay identity.
# 2. The read-only doctor in hermetic replay mode against a golden matrix.
# 3. Static audit: the observer stays pure (no SDL/env/dl/devices) and the
#    doctor never grabs, injects or opens a device for writing.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXINPUT=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-observe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "nxinput_observe=FAIL $*" >&2; exit 1; }

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
      -o "$WORK/observe-$tag" \
      "$HERE/test_observe.c" "$NXINPUT/src/nxinput_observe.c" ||
      fail "test_observe did not compile cleanly ($compiler $san)"
    "$WORK/observe-$tag" || fail "test_observe ($compiler $san)"
  done
done

# ------------------------------------------------------------------ doctor
"$CC" "${STRICT[@]}" -o "$WORK/doctor" "$HERE/nxinput_doctor.c" ||
  fail "nxinput-doctor did not compile cleanly"

cat > "$WORK/replay.txt" <<'REPLAY'
key 0x130 1
key 0x130 0
key 0x131 1
key 0x131 0
key 0x2c0 1
key 0x2c0 0
key 0x2c1 1
key 0x2c1 0
abs 0x00 2048
abs 0x00 4095
abs 0x00 0
abs 0x00 2048
abs 0x02 0
abs 0x02 255
abs 0x02 0
REPLAY
"$WORK/doctor" --replay "$WORK/replay.txt" > "$WORK/doctor.out" ||
  fail "the doctor replay run failed"
grep -Fq 'key BTN_SOUTH            canonical=A        press=1 release=1' \
  "$WORK/doctor.out" || fail "the doctor lost the BTN_SOUTH press/release"
grep -Fq 'key BTN_TRIGGER_HAPPY1   canonical=SELECT?  press=1 release=1' \
  "$WORK/doctor.out" || fail "the doctor lost the TRIGGER_HAPPY select"
grep -Fq 'abs ABS_X        canonical=LEFT_STICK.x       center=2048 min=0 max=4095 last=2048' \
  "$WORK/doctor.out" || fail "the doctor lost the stick center/min/max"
grep -Fq 'control L2           seen' "$WORK/doctor.out" ||
  fail "ABS_Z did not surface L2 in the canonical matrix"
grep -Fq 'control R2           never-pressed' "$WORK/doctor.out" ||
  fail "an untouched control must read never-pressed"
grep -Fq 'control START        seen' "$WORK/doctor.out" ||
  fail "TRIGGER_HAPPY2 did not surface START in the matrix"
if grep -Eq '/dev/|/home/|/roms/' "$WORK/doctor.out"; then
  fail "the doctor echoed a path"
fi

# ------------------------------------------------------------ static audit
! grep -Eq '(^|[^A-Z_])SDL_|getenv|dlopen|dlsym|fopen|open\(|socket|system\(' \
    "$NXINPUT/src/nxinput_observe.c" ||
  fail "nxinput_observe.c is not a pure observer"
! grep -Eq 'EVIOCGRAB|O_WRONLY|O_RDWR|uinput|write\(' \
    "$HERE/nxinput_doctor.c" ||
  fail "the doctor grabs, writes or injects"
for token in mali Mali panfrost rocknix ArkOS arkos muos NextOS RG40 R36 \
             rk3326 Adreno; do
  ! grep -Fq -- "$token" "$NXINPUT/src/nxinput_observe.c" ||
    fail "nxinput_observe.c mentions '$token'"
  ! grep -Fq -- "$token" "$NXINPUT/include/nxinput_observe.h" ||
    fail "nxinput_observe.h mentions '$token'"
done

echo "nxinput_observe=PASS module=1 doctor=1 static=1"
