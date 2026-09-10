#!/bin/bash
# Offline Godot 4.6.1 Mono + Tearscape graphics shim build. The outer recipe
# supplies only pinned, read-only inputs and caps this build at two workers.
set -euo pipefail

SOURCE_DIR=/src
TOOLCHAIN_DIR=/toolchain
BUSTER_SYSROOT=/buster
PORT_DIR=/port
OUT_DIR=/out
SCONS_WHEEL=/deps/SCons-4.8.1-py3-none-any.whl
BUILD_INPUTS=/build-causal-inputs.json
BUILD_FACTS=/tmp/tearscape-build-facts.json

TOOL_PREFIX="$TOOLCHAIN_DIR/bin/aarch64-none-linux-gnu-"
CC=/tmp/tools/aarch64-none-linux-gnu-gcc
CXX=/tmp/tools/aarch64-none-linux-gnu-g++
AR="${TOOL_PREFIX}ar"
RANLIB="${TOOL_PREFIX}ranlib"
STRIP="${TOOL_PREFIX}strip"

for required in "${TOOL_PREFIX}gcc" "${TOOL_PREFIX}g++" "$AR" "$RANLIB" \
	"$STRIP" "$SCONS_WHEEL" "$BUSTER_SYSROOT/usr/include/features.h" \
	"$BUSTER_SYSROOT/usr/include/alsa/asoundlib.h" \
	"$BUSTER_SYSROOT/usr/include/EGL/egl.h" \
	"$BUSTER_SYSROOT/usr/include/GLES2/gl2.h" \
	"$SOURCE_DIR/modules/mono/glue/runtime_interop.cpp" "$BUILD_INPUTS"; do
	[ -e "$required" ] || {
		printf 'missing offline build input: %s\n' "$required" >&2
		exit 1
	}
done

case ${TEARSCAPE_BUILD_JOBS:-2} in
	1|2) ;;
	*) printf 'TEARSCAPE_BUILD_JOBS must be 1 or 2\n' >&2; exit 1 ;;
esac
command -v nice >/dev/null || {
	printf 'missing low-priority build command: nice\n' >&2
	exit 1
}

export CC CXX AR RANLIB
export PYTHONDONTWRITEBYTECODE=1
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_CACHE_DIR=/tmp/pip-cache
export XDG_CACHE_HOME=/tmp/xdg-cache
export TMPDIR=/tmp
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH is required}
export ZERO_AR_DATE=1
export NX_SYSROOT="$BUSTER_SYSROOT"

# Recompute the exact snapshot mounted at /port and the fully prepared Godot
# tree before SCons mutates it.  These facts are carried into the immutable
# output receipt after the single build.
python3 - "$BUILD_INPUTS" "$PORT_DIR" "$SOURCE_DIR" "$BUILD_FACTS" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys

inputs_path, port, source, facts_path = map(Path, sys.argv[1:])

def load(path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise SystemExit(f"duplicate JSON key in {path}: {key}")
            value[key] = item
        return value
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)

def identity(base, selected=None):
    files = []
    if selected is None:
        def visit(directory):
            for entry in sorted(os.scandir(directory), key=lambda item: os.fsencode(item.name)):
                metadata = entry.stat(follow_symlinks=False)
                path = Path(entry.path)
                if stat.S_ISDIR(metadata.st_mode):
                    visit(path)
                elif stat.S_ISREG(metadata.st_mode):
                    files.append(path)
                else:
                    raise SystemExit(f"unsafe build input: {path}")
        visit(base)
        files.sort(key=lambda path: os.fsencode(path.relative_to(base).as_posix()))
    else:
        for relative in selected:
            path = base / relative
            metadata = path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                raise SystemExit(f"causal recipe input is not regular: {relative}")
            files.append(path)
        files.sort(key=lambda path: os.fsencode(path.relative_to(base).as_posix()))
    digest = hashlib.sha256()
    for path in files:
        payload = path.read_bytes()
        relative = path.relative_to(base).as_posix()
        mode = "100755" if path.stat().st_mode & 0o111 else "100644"
        digest.update(mode.encode() + b"\0" + relative.encode() + b"\0")
        digest.update(str(len(payload)).encode() + b"\0")
        digest.update(hashlib.sha256(payload).hexdigest().encode() + b"\n")
    return {"files": len(files), "sha256": digest.hexdigest()}

inputs = load(inputs_path)
if set(inputs) != {
        "causal_inputs", "port_head", "port_id", "schema",
        "schema_version", "source_date_epoch"}:
    raise SystemExit("causal build-input manifest fields are not canonical")
if (inputs.get("schema") != "org.nextos.tearscape.engine-build-inputs" or
        inputs.get("schema_version") != 1 or inputs.get("port_id") != "tearscape" or
        re.fullmatch(r"[0-9a-f]{40}", inputs.get("port_head", "")) is None or
        inputs.get("source_date_epoch") != int(os.environ["SOURCE_DATE_EPOCH"])):
    raise SystemExit("causal build-input manifest identity differs")
causal = inputs.get("causal_inputs")
if not isinstance(causal, dict) or set(causal) != {"recipe", "source_tree"}:
    raise SystemExit("causal build-input identities are malformed")
recipe = causal.get("recipe")
if not isinstance(recipe, dict) or set(recipe) != {"files", "paths", "sha256"}:
    raise SystemExit("causal recipe identity is malformed")
paths = recipe.get("paths")
if (not isinstance(paths, list) or paths != sorted(paths) or
        len(paths) != len(set(paths)) or not all(isinstance(path, str) for path in paths)):
    raise SystemExit("causal recipe paths are not canonical")
if identity(port, paths) != {"files": recipe.get("files"), "sha256": recipe.get("sha256")}:
    raise SystemExit("mounted causal recipe differs from its frozen identity")
if identity(port / "src") != causal.get("source_tree"):
    raise SystemExit("mounted port source differs from its frozen identity")
facts = dict(inputs)
facts["prepared_source_tree"] = identity(source)
facts_path.write_text(json.dumps(facts, indent=2, sort_keys=True) + "\n", encoding="utf-8")
facts_path.chmod(0o400)
PY

# Keep the canonical live GPTK boundary and the port-owned ACK sinks visible
# after --strip-unneeded. nxrelease ties its external input-proof receipt to
# these exact symbols in the final engine ELF.
NX_GPTK_EXPORTS="-Wl,--export-dynamic-symbol=nxinput_gptk_load_at"
NX_GPTK_EXPORTS+=" -Wl,--export-dynamic-symbol=nxinput_gptk_load_receipt_json"
NX_GPTK_EXPORTS+=" -Wl,--export-dynamic-symbol=nxinput_gptk_parse"
NX_GPTK_EXPORTS+=" -Wl,--export-dynamic-symbol=nxinput_gptk_decide"
NX_GPTK_EXPORTS+=" -Wl,--export-dynamic-symbol=nxinput_gptk_control_name"
for symbol in \
	nxinput_gptk_live_init nxinput_gptk_live_register \
	nxinput_gptk_live_register_vector nxinput_gptk_live_seal \
	nxinput_gptk_live_set_context nxinput_gptk_live_clear_context \
	nxinput_gptk_live_clear_context_checked nxinput_gptk_live_is_fatal \
	nxinput_gptk_live_context_epoch nxinput_gptk_live_context_source \
	nxinput_gptk_live_should_consume nxinput_gptk_live_feed \
	nxinput_gptk_live_feed_vector nxinput_gptk4_load_project_at \
	nxinput_gptk4_bridge_receipt_json nxinput_provider_declare_static \
	nxc6_declare_static_provider nxinput_gptk_runtime_marker \
		nxinput_gptk_event_evidence_schema tearscape_gptk_inputmap_sink \
		tearscape_gptk_inputmap_vector_sink tearscape_gptk_quit_sink \
		tearscape_gptk_resolve_context nxgl_frame_proof_before_present \
		nxgl_frame_proof_publish nxgl_frame_proof_launch_receipt \
		nxgl_frame_proof_set_resolver nxgl_frame_proof_set_video_context \
		nxgl_frame_proof_is_fatal nxgl_frame_proof_consume_fatal \
		nxinput_pm_convert_joydev_mapping nxinput_pm_normalize_source; do
	NX_GPTK_EXPORTS+=" -Wl,--export-dynamic-symbol=$symbol"
done

mkdir -p /tmp/python /tmp/tools "$OUT_DIR"
python3 -m pip install --no-index --no-deps --no-compile \
	--target /tmp/python "$SCONS_WHEEL" >/dev/null
ln -s /bin/true /tmp/tools/pkg-config
ln -s /port/recipes/compiler_wrapper.sh "$CC"
ln -s /port/recipes/compiler_wrapper.sh "$CXX"
ln -s "$AR" /tmp/tools/ar
ln -s "$RANLIB" /tmp/tools/ranlib
export PYTHONPATH=/tmp/python
export PATH="/tmp/tools:$TOOLCHAIN_DIR/bin:/usr/local/bin:/usr/bin:/bin"

printf 'offline compiler: '
"$CXX" --version | sed -n '1p'
printf 'offline sysroot: %s\n' "$BUSTER_SYSROOT"
printf 'source epoch: %s\n' "$SOURCE_DATE_EPOCH"

cd "$SOURCE_DIR"
nice -n 10 python3 -m SCons \
	--jobs="${TEARSCAPE_BUILD_JOBS:-2}" \
	platform=linuxbsd target=template_release arch=arm64 \
	fbdev=yes x11=no wayland=no vulkan=no opengl3=yes use_sowrap=yes \
	udev=yes fontconfig=no dbus=no speechd=no touch=no pulseaudio=no alsa=yes \
	disable_3d=no deprecated=yes minizip=yes brotli=yes \
	module_mono_enabled=yes \
	module_glslang_enabled=no module_raycast_enabled=no \
	module_lightmapper_rd_enabled=no module_csg_enabled=no \
	module_gridmap_enabled=no module_openxr_enabled=no module_webxr_enabled=no \
	lto=none debug_symbols=no use_static_cpp=yes progress=no verbose=no \
	ccflags="-fPIE -fstack-protector-strong -ffile-prefix-map=$SOURCE_DIR=. -fdebug-prefix-map=$SOURCE_DIR=. -fno-record-gcc-switches" \
	linkflags="-pie -Wl,--as-needed -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,--build-id=sha1 $NX_GPTK_EXPORTS" \
	CC="$CC" CXX="$CXX" LINK="$CXX" AR="$AR" RANLIB="$RANLIB"

ENGINE_BIN="$SOURCE_DIR/bin/godot.linuxbsd.template_release.arm64.mono"
[ -x "$ENGINE_BIN" ] || {
	printf 'Godot output not found: %s\n' "$ENGINE_BIN" >&2
	exit 1
}
install -m 0755 "$ENGINE_BIN" "$OUT_DIR/tearscape-nextos"
"$STRIP" --strip-unneeded "$OUT_DIR/tearscape-nextos"

CC="$CC" OUT="$OUT_DIR/shimlib" "$PORT_DIR/src/shim/build_shim.sh"
"$STRIP" --strip-unneeded "$OUT_DIR/shimlib/libEGL.so" \
	"$OUT_DIR/shimlib/libGLESv2.so"

python3 - "$OUT_DIR/tearscape-nextos" "$OUT_DIR/shimlib/libEGL.so" \
	"$OUT_DIR/shimlib/libGLESv2.so" <<'PY'
import os
import sys

epoch = int(os.environ["SOURCE_DATE_EPOCH"])
for path in sys.argv[1:]:
    os.utime(path, (epoch, epoch), follow_symlinks=False)
PY

python3 - "$BUILD_FACTS" "$OUT_DIR" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import sys

facts_path = Path(sys.argv[1])
out = Path(sys.argv[2])
facts = json.loads(facts_path.read_text(encoding="utf-8"))

def identity(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"sha256": digest.hexdigest(), "size": path.stat().st_size}

outputs = {
    relative: identity(out / relative)
    for relative in (
        "tearscape-nextos", "shimlib/libEGL.so", "shimlib/libGLESv2.so",
    )
}
receipt = {
    "build": {
        "builder_image_id": os.environ["TEARSCAPE_BUILDER_IMAGE_ID"],
        "clean_build_count": 1,
        "network": "none",
        "priority": "nice-10",
        "workers": int(os.environ["TEARSCAPE_BUILD_JOBS"]),
    },
    "causal_inputs": facts["causal_inputs"],
    "outputs": outputs,
    "port_head": facts["port_head"],
    "port_id": "tearscape",
    "prepared_source_tree": facts["prepared_source_tree"],
    "schema": "org.nextos.tearscape.engine-build-receipt",
    "schema_version": 1,
    "source_date_epoch": facts["source_date_epoch"],
}
payload = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode()
destination = out / "BUILD-RECEIPT.json"
descriptor = os.open(
    destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600,
)
try:
    os.write(descriptor, payload)
    os.fsync(descriptor)
    os.fchmod(descriptor, 0o444)
finally:
    os.close(descriptor)
PY
