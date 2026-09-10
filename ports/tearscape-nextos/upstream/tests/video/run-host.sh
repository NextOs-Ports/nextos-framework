#!/bin/bash
set -euo pipefail

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$TEST_DIR/../.." && pwd -P)
WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT

bash "$TEST_DIR/test_crt_disabled.sh"
bash "$TEST_DIR/test_nextossettings.sh"
python3 "$TEST_DIR/test_firmware_provider.py"

ran=0
for compiler in gcc clang; do
	command -v "$compiler" >/dev/null || continue
	ran=$((ran + 1))
	"$compiler" -std=c99 -Wall -Wextra -Werror \
		-I"$PORT_DIR/src/shim/include" \
		"$TEST_DIR/test_mali450_frame_proof_state.c" \
		-o "$WORK_DIR/test-$compiler"
	"$WORK_DIR/test-$compiler"

	# V5: the aspect DECISION moved to nxcompat_video (framework authority), so
	# what this port proves is its TRANSLATION plus the fact that the decision
	# really comes from there. test_aspect_policy.c tested the port's own copy
	# of the rule and was retired with it.
	"$compiler" -std=c99 -Wall -Wextra -Werror \
		-I"$PORT_DIR/src/godot_engine/v4-universal/platform/linuxbsd/fbdev" \
		"$TEST_DIR/test_aspect_translation.c" \
		"$PORT_DIR/src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxcompat_video.c" \
		-o "$WORK_DIR/aspect-$compiler"
	"$WORK_DIR/aspect-$compiler" | tail -n 1 | grep -q 'tearscape-aspect-translation: OK'

	"$compiler" -std=c99 -Wall -Wextra -Werror \
		-I"$PORT_DIR/src/shim/include" \
		"$TEST_DIR/test_mali450_frame_proof_e2e.c" \
		"$PORT_DIR/src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.c" \
		-ldl -o "$WORK_DIR/e2e-$compiler"
	proof_dir="$WORK_DIR/proof-$compiler"
	mkdir -m 0700 "$proof_dir"
	receipt="$proof_dir/video.json"
	output=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT \
		NXLAUNCH_FRONTEND=1 \
		NXBOOTSTRAP_VIDEO_FILE="$receipt" \
		NXBOOTSTRAP_HEALTH_RUN_ID=tearscape-mali450-host-1 \
		NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
		NXBOOTSTRAP_HEALTH_PORT_ID=tearscape \
		"$WORK_DIR/e2e-$compiler")
	printf '%s\n' "$output"
	grep -q 'MALI450 FRAME-PROOF E2E: physical_es3_queries=0 reads=1' <<<"$output"
	# 0.2.16: a live resize after context registration reaches the receipt.
	grep -q 'VIDEO: window=128x96 driver=mali ' <<<"$output"
	grep -q 'verdict=OK reason=none' <<<"$output"
	grep -q '"verdict":"OK","reason":"non-black"' "$receipt"

	"$compiler" -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
		-Wno-empty-body -ffunction-sections -fdata-sections \
		-I"$PORT_DIR/src/shim/include" \
		"$TEST_DIR/test_mali450_delete_unbind_e2e.c" \
		"$PORT_DIR/src/shim/nx_gles3_core.c" \
		"$PORT_DIR/src/godot_engine/v4-universal/platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.c" \
		-Wl,--gc-sections -ldl -lm -o "$WORK_DIR/delete-unbind-$compiler"
	delete_proof_dir="$WORK_DIR/delete-proof-$compiler"
	mkdir -m 0700 "$delete_proof_dir"
	delete_receipt="$delete_proof_dir/video.json"
	delete_output=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT \
		NXLAUNCH_FRONTEND=1 \
		NXBOOTSTRAP_VIDEO_FILE="$delete_receipt" \
		NXBOOTSTRAP_HEALTH_RUN_ID=tearscape-mali450-delete-1 \
		NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
		NXBOOTSTRAP_HEALTH_PORT_ID=tearscape \
		"$WORK_DIR/delete-unbind-$compiler")
	printf '%s\n' "$delete_output"
	grep -q 'MALI450 DELETE-UNBIND E2E: reads=1 physical_es3_queries=0' <<<"$delete_output"
	grep -q '"verdict":"OK","reason":"non-black"' "$delete_receipt"
done

[ "$ran" -gt 0 ] || {
	printf 'MALI450 FRAME-PROOF: FAIL: neither gcc nor clang is available\n' >&2
	exit 1
}

# WAYLAND_GEOMETRY_PROOF: SDL2 window geometry authority against a fake SDL2.
bash "$TEST_DIR/run-geometry-host.sh"
