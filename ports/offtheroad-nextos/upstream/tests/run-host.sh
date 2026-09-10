#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO_DIR=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$WORK_DIR/game/defaults" "$WORK_DIR/cache"
cp "$PORT_DIR/defaults/NEXTOSCONTROLLERS.gptk" \
  "$WORK_DIR/game/defaults/NEXTOSCONTROLLERS.gptk"

cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/test_util_cache.c" "$PORT_DIR/src/util.c" \
  -o "$WORK_DIR/test-util-cache"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" -I"$REPO_DIR/framework/nxinput/include" \
  "$PORT_DIR/tests/test_gptk_adapter.c" \
  "$PORT_DIR/src/otr_gptk.c" \
  "$REPO_DIR/framework/nxinput/src/nxinput_gptk.c" \
  "$REPO_DIR/framework/nxinput/src/nxinput_gptk_loader.c" \
  "$REPO_DIR/framework/nxinput/src/nxinput_gptk_motion.c" \
  -lm -o "$WORK_DIR/test-gptk-adapter"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" -I"$REPO_DIR/framework/nxinput/include" \
  "$PORT_DIR/tests/test_exit_monitor.c" \
  "$PORT_DIR/src/otr_exit_monitor.c" \
  "$REPO_DIR/framework/nxinput/src/nxinput_exit_chord.c" \
  -pthread -o "$WORK_DIR/test-exit-monitor"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" -I"$REPO_DIR/framework/nxaudio/include" \
  "$PORT_DIR/tests/test_audio_recovery.c" \
  "$PORT_DIR/src/otr_audio_recovery.c" \
  "$REPO_DIR/framework/nxaudio/src/nxaudio_receipt.c" \
  -o "$WORK_DIR/test-audio-recovery"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
  -I"$REPO_DIR/framework/nxgl/include" \
  "$PORT_DIR/tests/test_graphics_receipt.c" \
  "$REPO_DIR/framework/nxgl/src/nxgl_graphics_contract.c" \
  -o "$WORK_DIR/test-graphics-receipt"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/test_egl_imports.c" \
  "$PORT_DIR/src/otr_egl_imports.c" \
  -ldl -o "$WORK_DIR/test-egl-imports"

cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror -fPIC -shared \
  "$PORT_DIR/tests/egl_provider_fixture.c" \
  -o "$WORK_DIR/libotr-egl-complete.so"
cc -D_GNU_SOURCE -DOTR_EGL_FIXTURE_MISSING_SWAP \
  -std=gnu11 -Wall -Wextra -Werror -fPIC -shared \
  "$PORT_DIR/tests/egl_provider_fixture.c" \
  -o "$WORK_DIR/libotr-egl-incomplete.so"
cc -D_GNU_SOURCE -DOTR_EGL_FIXTURE_NULL_CONTEXT \
  -std=gnu11 -Wall -Wextra -Werror -fPIC -shared \
  "$PORT_DIR/tests/egl_provider_fixture.c" \
  -o "$WORK_DIR/libotr-egl-null-context.so"

"$WORK_DIR/test-util-cache" "$WORK_DIR/cache"
OTR_SWAPPED_GPTK="$PORT_DIR/tests/fixtures/NEXTOSCONTROLLERS-swapped.gptk" \
  "$WORK_DIR/test-gptk-adapter" "$WORK_DIR/game"
"$WORK_DIR/test-exit-monitor"
"$WORK_DIR/test-audio-recovery"
"$WORK_DIR/test-graphics-receipt"
"$WORK_DIR/test-egl-imports" \
  "$WORK_DIR/libotr-egl-complete.so" \
  "$WORK_DIR/libotr-egl-incomplete.so" \
  "$WORK_DIR/libotr-egl-null-context.so"
python3 -B "$PORT_DIR/tests/test_static_contract.py"

printf 'offtheroad host promotion gates: PASS\n'
