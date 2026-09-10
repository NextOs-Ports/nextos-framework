#!/bin/sh
# Directed host regressions only: no package, archive, device or network.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORT=$(dirname "$HERE")
REPO=$(CDPATH= cd -- "$PORT/../.." && pwd)
WORK=${BT_TEST_WORK:-"${TMPDIR:-/tmp}/blossomtales-targeted-$$"}
trap 'rm -rf -- "$WORK"' EXIT HUP INT TERM
mkdir -p "$WORK"

${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror -I"$PORT/src" \
  "$PORT/src/fbo_compat.c" "$HERE/test_fbo_compat.c" \
  -o "$WORK/test_fbo_compat"
"$WORK/test_fbo_compat"

${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror \
  "$HERE/test_present_fbo_scan.c" \
  -o "$WORK/test_present_fbo_scan"
"$WORK/test_present_fbo_scan"

${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror \
  -I"$PORT/src" -I"$PORT/vendor/nxcompat/include" \
  "$PORT/src/present_policy.c" \
  "$PORT/vendor/nxcompat/src/nxcompat_settings.c" \
  "$PORT/vendor/nxcompat/src/nxcompat_video.c" \
  "$HERE/test_present_policy.c" -lm -o "$WORK/test_present_policy"
mkdir -p "$WORK/present-settings" "$WORK/present-fallback"
(cd "$WORK/present-settings" && "$WORK/test_present_policy" settings)
(cd "$WORK/present-fallback" && "$WORK/test_present_policy" fallback)

NXI="$PORT/vendor/nxinput"
V5="$REPO/framework/nxinput/tests/v5"
FLAGS="-std=c99 -O2 -Wall -Wextra -Werror -Wno-format-truncation -Wno-misleading-indentation -D_POSIX_C_SOURCE=200809L"
# shellcheck disable=SC2086
${CC:-cc} $FLAGS -I"$NXI/include" -I"$NXI/engine-glue" \
  "$V5/test_v5_seam.c" "$NXI/src/nxinput_sdl_seam.c" \
  "$NXI/src/nxinput_decision.c" "$NXI/src/nxinput_provider.c" \
  "$NXI/src/nxinput_sdl.c" "$NXI/src/nxinput_portmaster.c" \
  "$NXI/src/nxinput_sovereign.c" "$NXI/src/nxinput_authority.c" \
  "$NXI/src/nxinput_livedb.c" "$NXI/src/nxinput_godot.c" \
  -ldl -lm -lpthread -o "$WORK/test_v5_seam"
"$WORK/test_v5_seam" > "$WORK/seam.log"
tail -n 1 "$WORK/seam.log"

# shellcheck disable=SC2086
${CC:-cc} $FLAGS -I"$NXI/include" -I"$NXI/engine-glue" \
  "$V5/test_v5_padset.c" "$NXI/engine-glue/nxinput_padset.c" \
  "$NXI/src/nxinput_prerouter.c" "$NXI/src/nxinput_axis_calib.c" \
  -lm -o "$WORK/test_v5_padset"
"$WORK/test_v5_padset" > "$WORK/padset.log"
tail -n 1 "$WORK/padset.log"

python3 -B "$REPO/framework/nxinput/tests/v5/test_v5_stage_subsystem.py" \
  "$PORT/vendor/nxinput"

python3 -B "$HERE/test_v5_integration.py" "$PORT" "$REPO"
echo "BLOSSOM TARGETED TESTS: PASS"
