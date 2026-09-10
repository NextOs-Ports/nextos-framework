#!/bin/bash
# WAYLAND_GEOMETRY_PROOF host gate: the SDL2 window geometry authority against
# a fake firmware libSDL2-2.0.so.0 (dlopen'd through LD_LIBRARY_PATH), plus the
# gate consumer over the receipts it produces. No device, no real SDL, no Godot.
set -euo pipefail

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$TEST_DIR/../.." && pwd -P)
FBDEV_DIR="$PORT_DIR/src/godot_engine/v4-universal/platform/linuxbsd/fbdev"
WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT

ran=0
for compiler in g++ clang++; do
	command -v "$compiler" >/dev/null || continue
	ran=$((ran + 1))
	ccompiler=gcc
	[ "$compiler" = clang++ ] && ccompiler=clang
	command -v "$ccompiler" >/dev/null || continue

	build="$WORK_DIR/$compiler"
	mkdir -p "$build/full" "$build/minimal" "$build/out"
	"$ccompiler" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
		-Wl,-soname,libSDL2-2.0.so.0 \
		"$TEST_DIR/fake_sdl2.c" -o "$build/full/libSDL2-2.0.so.0"
	"$ccompiler" -std=c99 -Wall -Wextra -Werror -shared -fPIC -DFAKE_SDL_MINIMAL \
		-Wl,-soname,libSDL2-2.0.so.0 \
		"$TEST_DIR/fake_sdl2.c" -o "$build/minimal/libSDL2-2.0.so.0"
	"$compiler" -std=gnu++17 -Wall -Wextra -Werror \
		-I"$FBDEV_DIR" \
		"$TEST_DIR/test_sdl2_geometry_host.cpp" \
		"$FBDEV_DIR/nx_sdl2_geometry.cpp" \
		-ldl -o "$build/geometry-host"

	# The run-bound tuple is what the gate cross-checks against the video proof.
	env_common=(
		NXBOOTSTRAP_HEALTH_RUN_ID=tearscape-geometry-host-1
		NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef
		NXBOOTSTRAP_HEALTH_PORT_ID=tearscape
		NX_GEOMETRY_TEST_OUT="$build/out"
	)
	output=$(env "${env_common[@]}" LD_LIBRARY_PATH="$build/full" "$build/geometry-host" full)
	printf '%s\n' "$output"
	grep -q '^SDL2 GEOMETRY HOST (full): PASS failures=0$' <<<"$output"
	output=$(env "${env_common[@]}" LD_LIBRARY_PATH="$build/minimal" "$build/geometry-host" minimal)
	printf '%s\n' "$output"
	grep -q '^SDL2 GEOMETRY HOST (minimal): PASS failures=0$' <<<"$output"

	# Every receipt line is strict JSON with the schema; then the gate consumer
	# must accept wayland/kmsdrm/fbdev and refuse the timed-out receipt.
	python3 -B - "$build/out" "$PORT_DIR/recipes/make_geometry_proof.py" <<'PY'
import importlib.util, json, sys
from pathlib import Path
out = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("make_geometry_proof", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
required_init = {
    "schema", "kind", "provider", "video_driver", "app_id", "app_id_source",
    "raw_fb_w", "raw_fb_h", "display_w", "display_h", "requested_w",
    "requested_h", "configured_w", "configured_h", "drawable_w", "drawable_h",
    "fullscreen", "configure_events", "wait_ms", "timed_out", "run_id",
    "generation", "port_id",
}
required_resize = {"schema", "kind", "w", "h", "drawable_w", "drawable_h", "notified"}
for name in ("wayland", "kmsdrm", "timeout", "minimal", "fbdev"):
    lines = (out / f"{name}.jsonl").read_text(encoding="utf-8").splitlines()
    assert lines, name
    for line in lines:
        record = json.loads(line)
        assert record["schema"] == "nx-geometry-proof/1", name
        fields = required_init if record["kind"] == "init" else required_resize
        assert fields <= set(record), (name, sorted(fields - set(record)))
tuple_ = {"run_id": "tearscape-geometry-host-1", "generation": "0123456789abcdef"}
for name in ("wayland", "kmsdrm", "fbdev"):
    proof = module.read_geometry_proof(out / f"{name}.jsonl", **tuple_)
    assert proof["provider"] in ("sdl2", "fbdev"), name
wayland = module.read_geometry_proof(out / "wayland.jsonl", **tuple_)
assert (wayland["configured_w"], wayland["configured_h"]) == (1920, 1080)
assert wayland["resizes"] == 1 and wayland["resize_notified"] is True
try:
    module.read_geometry_proof(out / "timeout.jsonl", **tuple_)
except module.GeometryProofError as error:
    assert "timed_out" in str(error), error
else:
    raise SystemExit("timed-out receipt was accepted")
try:
    module.read_geometry_proof(out / "wayland.jsonl", run_id="other", generation="0123456789abcdef")
except module.GeometryProofError as error:
    assert "run_id" in str(error), error
else:
    raise SystemExit("run_id mismatch was accepted")
print("GEOMETRY RECEIPTS: PASS")
PY
done

[ "$ran" -gt 0 ] || {
	printf 'SDL2 GEOMETRY HOST: FAIL: neither g++ nor clang++ is available\n' >&2
	exit 1
}
printf 'SDL2 GEOMETRY HOST GATE: PASS\n'
