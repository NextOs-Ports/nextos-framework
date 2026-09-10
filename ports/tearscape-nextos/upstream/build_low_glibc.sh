#!/bin/bash
# Deterministic frozen-input/offline universal Tearscape engine build. The
# 0.2.12 note: the first claim for these logical inputs (key d8e42fe6...)
# was interrupted externally mid-build (twice: claims d8e42fe6, a7efcd84);
# this byte is the documented successor
# so the one-shot ledger stays honest without reusing a consumed claim.
# official Godot, SDL and toolchain inputs are content-addressed; no owner game
# data enters. This recipe intentionally performs one build, not a second
# reproducibility build.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BUILD_ROOT=${TEARSCAPE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/tearscape-nextos}
DOWNLOAD_DIR="$BUILD_ROOT/downloads"
FINAL_OUT=${TEARSCAPE_FINAL_OUT:-}
BUILD_JOBS=${TEARSCAPE_BUILD_JOBS:-2}
FETCH_INPUTS=${TEARSCAPE_FETCH_INPUTS:-0}

case $BUILD_JOBS in 1|2) ;; *) printf 'TEARSCAPE_BUILD_JOBS must be 1 or 2\n' >&2; exit 1 ;; esac
[ "${TEARSCAPE_REPRODUCIBLE_CHECK:-0}" = 0 ] || {
	printf 'a second engine build is forbidden; freeze a successor commit first\n' >&2
	exit 1
}

GODOT_COMMIT=14d19694e0c88a3f9e82d899a0400f27a24c176e
GODOT_ARCHIVE="godot-$GODOT_COMMIT.tar.gz"
GODOT_URL="https://github.com/godotengine/godot/archive/$GODOT_COMMIT.tar.gz"
GODOT_SHA256=b29af30d344afa50e44773e5347bdd3ae3151527d871f7dcc373ba8f3b52f893
SDL_COMMIT=f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc
SDL_ARCHIVE="SDL-$SDL_COMMIT.tar.gz"
SDL_URL="https://github.com/libsdl-org/SDL/archive/$SDL_COMMIT.tar.gz"
SDL_SHA256=7f80bce51b7179bab472e75b90ce8ac06b162b3da8e19feadecee5acea07dbfb
NXINPUT_FRAMEWORK_COMMIT=ba1da0e7b0482c4dbad76b7e36afe04de57fa5c3
NXINPUT_FRAMEWORK_STATE=candidate-commit-pinned
NXINPUT_COMPONENT_VERSION=0.11.8
NXINPUT_TREE_FILES=306
NXINPUT_TREE_SHA256=8e7f6dabd445a6b8d428685ea73f02291e7fda667a4c16f955dd50b7b53dd2b4
NXINPUT_FRAMEWORK_TAG=nxinput-v0.9.0
NXINPUT_FRAMEWORK_TAG_OBJECT=32ed6553e16bc4195baed988ea9a9d3849fd86b1
NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256=2accbb023971fa47c4e73d23ee2e46472572b857f94911cabca3d9cf814c95b2

TOOLCHAIN_NAME=gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu
TOOLCHAIN_ARCHIVE="$TOOLCHAIN_NAME.tar.xz"
TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu-a/10.3-2021.07/binrel/$TOOLCHAIN_ARCHIVE"
TOOLCHAIN_SHA256=1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84
SCONS_WHEEL=SCons-4.8.1-py3-none-any.whl
SCONS_URL=https://files.pythonhosted.org/packages/b8/a7/823091100c88d7d992c8c608d82a88ed59e227d13f8ccb33ea7a96d43d51/SCons-4.8.1-py3-none-any.whl
SCONS_SHA256=a4c3b434330e2d7d975002fd6783284ba348bf394db94c8f83fdc5bf69cdb8d7
SYSROOT_ARCHIVE=mmw-buster-aarch64-sysroot.tar.xz
SYSROOT_SHA256=484c85086c5e03067e633d98e1617cd5b6b21a60a596a7cdcf6e42524d363e04
BUILDER_IMAGE='python@sha256:57cd7c3a7a273101a6485ba99423ee568157882804b1124b4dd04266317710de'
BUILDER_IMAGE_ID='sha256:57cd7c3a7a273101a6485ba99423ee568157882804b1124b4dd04266317710de'
SOURCE_DATE_EPOCH=1771254831
export SOURCE_DATE_EPOCH
BUILD_RECIPE_PATHS=(
	build_low_glibc.sh
	recipes/audit_low_glibc.sh
	recipes/compiler_wrapper.sh
	recipes/gcc10_buster_compat.h
	recipes/low_glibc_container.sh
	recipes/vendor_sdl_3_2_30.sh
)

for command in awk basename chmod cp dirname docker find git grep gzip id install \
	mkdir nice patch python3 rm sed sha256sum sort tail tar tee xz; do
	command -v "$command" >/dev/null || { printf 'missing command: %s\n' "$command" >&2; exit 1; }
done

REPOSITORY=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
PORT_PREFIX=$(git -C "$PORT_DIR" rev-parse --show-prefix)
PORT_HEAD=$(git -C "$REPOSITORY" rev-parse HEAD)
case $PORT_HEAD in
	????????????????????????????????????????) ;;
	*) printf 'port HEAD is not a full 40-hex commit\n' >&2; exit 1 ;;
esac
case $PORT_HEAD in *[!0-9a-f]*) printf 'port HEAD is not lowercase hex\n' >&2; exit 1 ;; esac

require_clean_port() {
	local status
	status=$(git -C "$REPOSITORY" status --porcelain=v1 --untracked-files=all -- "$PORT_PREFIX")
	[ -z "$status" ] || {
		printf 'refusing one-shot build from dirty port inputs:\n%s\n' "$status" >&2
		exit 1
	}
}
require_clean_port

# The engine may consume its one-shot claim only after the framework release
# and every committed port-side pin agree on the exact component identity.
# Two states exist (mission 11, layout authority):
#   component-tag-pinned    -- an immutable annotated tag names the release;
#   candidate-commit-pinned -- NO tag exists or may be created. The pin is
#                              the exact commit + component VERSION + tree
#                              identity, and the artifact stays a CANDIDATE:
#                              it can never claim physical support, publish
#                              or promote (the packager refuses).
# This check intentionally runs before downloads, staging and the ledger.
if [ "$NXINPUT_FRAMEWORK_STATE" = component-tag-pinned ]; then
	NXINPUT_TAG_REF="refs/tags/$NXINPUT_FRAMEWORK_TAG"
	NXINPUT_TAG_TYPE=$(git -C "$REPOSITORY" cat-file -t "$NXINPUT_TAG_REF" 2>/dev/null || true)
	[ "$NXINPUT_TAG_TYPE" = tag ] || {
		printf 'nxinput release is not an annotated tag: %s\n' "$NXINPUT_FRAMEWORK_TAG" >&2
		exit 1
	}
	NXINPUT_TAG_OBJECT=$(git -C "$REPOSITORY" rev-parse "$NXINPUT_TAG_REF" 2>/dev/null || true)
	[ "$NXINPUT_TAG_OBJECT" = "$NXINPUT_FRAMEWORK_TAG_OBJECT" ] || {
		printf 'nxinput tag object differs from the immutable pin\n' >&2
		exit 1
	}
	NXINPUT_TAG_COMMIT=$(git -C "$REPOSITORY" rev-parse "$NXINPUT_TAG_REF^{commit}" 2>/dev/null || true)
	[ "$NXINPUT_TAG_COMMIT" = "$NXINPUT_FRAMEWORK_COMMIT" ] || {
		printf 'nxinput tag does not resolve to the pinned framework commit\n' >&2
		exit 1
	}
elif [ "$NXINPUT_FRAMEWORK_STATE" = candidate-commit-pinned ]; then
	NXINPUT_PINNED_VERSION=$(git -C "$REPOSITORY" show 		"$NXINPUT_FRAMEWORK_COMMIT:framework/nxinput/VERSION" 2>/dev/null || true)
	[ "$NXINPUT_PINNED_VERSION" = "$NXINPUT_COMPONENT_VERSION" ] || {
		printf 'candidate pin: nxinput VERSION at the pinned commit is not %s\n' 			"$NXINPUT_COMPONENT_VERSION" >&2
		exit 1
	}
else
	printf 'unknown framework pin state: %s\n' "$NXINPUT_FRAMEWORK_STATE" >&2
	exit 1
fi
python3 - "$REPOSITORY" "$PORT_HEAD" "$PORT_PREFIX" \
	"$NXINPUT_FRAMEWORK_COMMIT" "$NXINPUT_FRAMEWORK_TAG" \
	"$NXINPUT_FRAMEWORK_TAG_OBJECT" "$NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256" \
	"$NXINPUT_FRAMEWORK_STATE" "$NXINPUT_COMPONENT_VERSION" \
	"$NXINPUT_TREE_FILES" "$NXINPUT_TREE_SHA256" <<'PY'
import ast
import hashlib
import json
from pathlib import Path
import subprocess
import sys

(repository, head, prefix, framework_commit, framework_tag,
 framework_tag_object, framework_tag_object_sha256, framework_state,
 component_version, tree_files, tree_sha256) = sys.argv[1:]

def committed(relative):
    result = subprocess.run(
        ["git", "-C", repository, "show", f"{head}:{prefix}{relative}"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise SystemExit(f"missing committed release input: {relative}")
    return result.stdout

def released(relative):
    result = subprocess.run(
        ["git", "-C", repository, "show",
         f"{framework_commit}:framework/nxinput/{relative}"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise SystemExit(f"missing released nxinput input: {relative}")
    return result.stdout

def strict_json(relative):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise SystemExit(f"duplicate JSON key in {relative}: {key}")
            value[key] = item
        return value
    return json.loads(committed(relative).decode("utf-8"), object_pairs_hook=unique)

public = strict_json("FRAMEWORK-PIN.json")
private = strict_json("recipes/framework-release-pin-v1.json")
if public != private:
    raise SystemExit("committed public and recipe framework pins differ")
if public.get("framework_commit") != framework_commit:
    raise SystemExit("committed framework pin names the wrong commit")
if public.get("framework_state") != framework_state:
    raise SystemExit("committed framework pin state differs from the recipe")
if framework_state == "component-tag-pinned":
    if public.get("component_tags") != {"nxinput": framework_tag}:
        raise SystemExit("committed framework pin names the wrong nxinput tag")
    if public.get("component_tag_objects") != {"nxinput": framework_tag_object}:
        raise SystemExit("committed framework pin names the wrong nxinput tag object")
    if public.get("component_tag_object_sha256") != {
            "nxinput": framework_tag_object_sha256}:
        raise SystemExit("committed framework pin names the wrong nxinput tag digest")
    tag_payload = subprocess.run(
        ["git", "-C", repository, "cat-file", "tag", f"refs/tags/{framework_tag}"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    logical_tag_object = (
        b"tag " + str(len(tag_payload.stdout)).encode() + b"\0" + tag_payload.stdout
    )
    if (tag_payload.returncode != 0 or
            hashlib.sha256(logical_tag_object).hexdigest() != framework_tag_object_sha256):
        raise SystemExit("nxinput tag object differs from its SHA-256 pin")
elif framework_state == "candidate-commit-pinned":
    # NO tag exists or may be created for a candidate. The identity is the
    # exact commit + component VERSION + tree digest, and the state itself
    # forbids publication: the packager refuses to promote it.
    if public.get("component_tags") not in (None, {}):
        raise SystemExit("a candidate pin must not name component tags")
    if released("VERSION").decode().strip() != component_version:
        raise SystemExit("candidate pin: released nxinput VERSION mismatch")
else:
    raise SystemExit(f"unknown framework pin state: {framework_state}")
if public.get("components", {}).get("nxinput") != component_version:
    raise SystemExit(
        f"committed framework pin does not select nxinput {component_version}")
nxinput_roots = [
    item for item in public.get("roots", [])
    if item.get("path") == "framework/nxinput"
]
if nxinput_roots != [{
        "files": int(tree_files),
        "path": "framework/nxinput",
        "tree_sha256": tree_sha256,
}]:
    raise SystemExit("committed nxinput tree identity is not the released one")

def constants(relative):
    tree = ast.parse(committed(relative).decode("utf-8"), filename=relative)
    result = {}
    for statement in tree.body:
        if (isinstance(statement, ast.Assign) and len(statement.targets) == 1 and
                isinstance(statement.targets[0], ast.Name)):
            try:
                result[statement.targets[0].id] = ast.literal_eval(statement.value)
            except (ValueError, TypeError):
                pass
    return result

refresh = constants("recipes/refresh_release_inputs.py")
packager = constants("recipes/build_public_byo.py")
for label, values in (("refresh", refresh), ("packager", packager)):
    if values.get("FRAMEWORK_COMMIT") != framework_commit:
        raise SystemExit(f"committed {label} uses the wrong framework commit")
    if values.get("NXINPUT_FRAMEWORK_TAG") != framework_tag:
        raise SystemExit(f"committed {label} uses the wrong nxinput tag")
    if values.get("NXINPUT_FRAMEWORK_TAG_OBJECT") != framework_tag_object:
        raise SystemExit(f"committed {label} uses the wrong nxinput tag object")
    if values.get("NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256") != framework_tag_object_sha256:
        raise SystemExit(f"committed {label} uses the wrong nxinput tag digest")
    if values.get("NXINPUT_0_9_PIN_PENDING") is not False:
        raise SystemExit(f"committed {label} still blocks nxinput 0.9.0")
if refresh.get("NXINPUT_OWNER_COMMIT") != framework_commit:
    raise SystemExit("committed refresh uses the wrong nxinput owner commit")

vendor_sources = {
    "nxinput_authority.c": "src/nxinput_authority.c",
    "nxinput_authority.h": "include/nxinput_authority.h",
    "nxinput_godot.c": "src/nxinput_godot.c",
    "nxinput_godot.h": "include/nxinput_godot.h",
    "nxinput_godot_runtime.h": "include/nxinput_godot_runtime.h",
    "nxinput_gptk.c": "src/nxinput_gptk.c",
    "nxinput_gptk.h": "include/nxinput_gptk.h",
    "nxinput_gptk_live.c": "src/nxinput_gptk_live.c",
    "nxinput_gptk_live.h": "include/nxinput_gptk_live.h",
    "nxinput_gptk_loader.c": "src/nxinput_gptk_loader.c",
    "nxinput_gptk_loader.h": "include/nxinput_gptk_loader.h",
    "nxinput_gptk_motion.c": "src/nxinput_gptk_motion.c",
    "nxinput_gptk_motion.h": "include/nxinput_gptk_motion.h",
    "nxinput_portmaster.c": "src/nxinput_portmaster.c",
    "nxinput_portmaster.h": "include/nxinput_portmaster.h",
    "nxinput_sdl.c": "src/nxinput_sdl.c",
    "nxinput_sdl.h": "include/nxinput_sdl.h",
    "nxinput_sdl_seam.c": "src/nxinput_sdl_seam.c",
    "nxinput_sdl_seam.h": "include/nxinput_sdl_seam.h",
    "nxinput_sovereign.c": "src/nxinput_sovereign.c",
    "nxinput_sovereign.h": "include/nxinput_sovereign.h",
    "nxc6_glue.c": "engine-glue/nxc6_glue.c",
    "nxc6_glue.h": "engine-glue/nxc6_glue.h",
    "sdl3-3.2.30-nxc6-seam.patch": "engine-patches/sdl3-3.2.30-nxc6-seam.patch",
    "nxinput_gptk_preinit.c": "src/nxinput_gptk_preinit.c",
    "nxinput_gptk_preinit.h": "include/nxinput_gptk_preinit.h",
    "nxinput_livedb.c": "src/nxinput_livedb.c",
    "nxinput_livedb.h": "include/nxinput_livedb.h",
    "nxinput_provider.c": "src/nxinput_provider.c",
    "nxinput_provider.h": "include/nxinput_provider.h",
    "nxinput_provider_linux.c": "src/nxinput_provider_linux.c",
    "nxinput_provider_linux.h": "include/nxinput_provider_linux.h",
    "nxinput_sha256.c": "src/nxinput_sha256.c",
    "nxinput_sha256.h": "include/nxinput_sha256.h",
    "nxinput_translate.c": "src/nxinput_translate.c",
    "nxinput_translate.h": "include/nxinput_translate.h",
    "nxinput_decision.c": "src/nxinput_decision.c",
    "nxinput_decision.h": "include/nxinput_decision.h",
    "nxinput_gptk4.c": "src/nxinput_gptk4.c",
    "nxinput_gptk4.h": "include/nxinput_gptk4.h",
    "nxinput_gptk4_bridge.c": "src/nxinput_gptk4_bridge.c",
    "nxinput_gptk4_bridge.h": "include/nxinput_gptk4_bridge.h",
    # V5: the universal schema-4 pre-init replaces the sixty lines this port
    # used to carry inside the Godot driver (nxinput 0.11.8, M1a item 1).
    "nxinput_gptk4_preinit.c": "src/nxinput_gptk4_preinit.c",
    "nxinput_gptk4_preinit.h": "include/nxinput_gptk4_preinit.h",
    "nx-gptk4-host-gate.c": "tools/nx-gptk4-host-gate.c",
}
vendor_tree = subprocess.run(
    ["git", "-C", repository, "ls-tree", "--name-only",
     f"{head}:{prefix}src/nxinput"],
    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    check=False, text=True,
)
if (vendor_tree.returncode != 0 or
        sorted(vendor_tree.stdout.splitlines()) != sorted(vendor_sources)):
    raise SystemExit("committed nxinput vendor file set differs from the release map")
if released("VERSION").decode().strip() != component_version:
    raise SystemExit(f"released nxinput version is not {component_version}")
for local, upstream in vendor_sources.items():
    if committed(f"src/nxinput/{local}") != released(upstream):
        raise SystemExit(f"vendored nxinput differs from released tag: {local}")
PY

[ -n "$FINAL_OUT" ] || {
	printf 'TEARSCAPE_FINAL_OUT must name a new, explicit output directory\n' >&2
	exit 1
}
case $FINAL_OUT in
	/*) ;;
	*) printf 'TEARSCAPE_FINAL_OUT must be absolute\n' >&2; exit 1 ;;
esac
FINAL_OUT_NAME=$(basename -- "$FINAL_OUT")
FINAL_OUT_PARENT_INPUT=$(dirname -- "$FINAL_OUT")
case $FINAL_OUT_NAME in ''|.|..) printf 'invalid TEARSCAPE_FINAL_OUT\n' >&2; exit 1 ;; esac
FINAL_OUT_PARENT=$(CDPATH= cd -- "$FINAL_OUT_PARENT_INPUT" && pwd -P) || {
	printf 'TEARSCAPE_FINAL_OUT parent must already exist\n' >&2
	exit 1
}
FINAL_OUT="$FINAL_OUT_PARENT/$FINAL_OUT_NAME"
[ ! -e "$FINAL_OUT" ] && [ ! -L "$FINAL_OUT" ] || {
	printf 'TEARSCAPE_FINAL_OUT already exists; no replacement is allowed: %s\n' "$FINAL_OUT" >&2
	exit 1
}
case "$FINAL_OUT/" in
	"$PORT_DIR"/*) printf 'TEARSCAPE_FINAL_OUT must be outside the port checkout\n' >&2; exit 1 ;;
esac

file_sha256() { sha256sum "$1" | awk '{print $1}'; }
fetch_input() {
	local url=$1 destination=$2 expected=$3
	if [ -f "$destination" ]; then
		[ "$(file_sha256 "$destination")" = "$expected" ] || {
			printf 'cached input digest mismatch: %s\n' "$destination" >&2; exit 1;
		}
		return
	fi
	[ "$FETCH_INPUTS" = 1 ] || { printf 'missing pinned input: %s\n' "$destination" >&2; exit 1; }
	curl -fSL --proto '=https' --tlsv1.2 -o "$destination.part" "$url"
	mv "$destination.part" "$destination"
	[ "$(file_sha256 "$destination")" = "$expected" ] || { printf 'download digest mismatch\n' >&2; exit 1; }
}

mkdir -p "$DOWNLOAD_DIR"
fetch_input "$GODOT_URL" "$DOWNLOAD_DIR/$GODOT_ARCHIVE" "$GODOT_SHA256"
fetch_input "$SDL_URL" "$DOWNLOAD_DIR/$SDL_ARCHIVE" "$SDL_SHA256"
fetch_input "$TOOLCHAIN_URL" "$DOWNLOAD_DIR/$TOOLCHAIN_ARCHIVE" "$TOOLCHAIN_SHA256"
fetch_input "$SCONS_URL" "$DOWNLOAD_DIR/$SCONS_WHEEL" "$SCONS_SHA256"
[ -f "$DOWNLOAD_DIR/$SYSROOT_ARCHIVE" ] && \
	[ "$(file_sha256 "$DOWNLOAD_DIR/$SYSROOT_ARCHIVE")" = "$SYSROOT_SHA256" ] || {
	printf 'missing or invalid pinned Buster sysroot: %s\n' "$DOWNLOAD_DIR/$SYSROOT_ARCHIVE" >&2
	exit 1
}

actual_image_id=$(docker image inspect --format '{{.Id}}' "$BUILDER_IMAGE" 2>/dev/null || true)
[ "$actual_image_id" = "$BUILDER_IMAGE_ID" ] || {
	printf 'wrong builder image ID: %s\n' "${actual_image_id:-none}" >&2
	exit 1
}

WORK_DIR=$(mktemp -d "$BUILD_ROOT/work.XXXXXX")
cleanup() {
	[ -d "$WORK_DIR" ] && chmod -R u+w "$WORK_DIR" 2>/dev/null || true
	[ -d "$WORK_DIR" ] && rm -rf -- "$WORK_DIR" || true
}
trap cleanup EXIT

# Freeze every port-owned byte that can affect this one engine build directly
# from the committed tree. Ignored files, skip-worktree flags, untracked files
# and concurrent live-checkout edits can never enter the container snapshot.
SNAPSHOT_ROOT="$WORK_DIR/repository-snapshot"
mkdir -m 0755 "$SNAPSHOT_ROOT"
ARCHIVE_PATHS=("${PORT_PREFIX}src")
for relative in "${BUILD_RECIPE_PATHS[@]}"; do
	ARCHIVE_PATHS+=("${PORT_PREFIX}${relative}")
done
git -C "$REPOSITORY" archive --format=tar "$PORT_HEAD" -- \
	"${ARCHIVE_PATHS[@]}" | tar -xf - -C "$SNAPSHOT_ROOT"
BUILD_PORT_DIR="$SNAPSHOT_ROOT/${PORT_PREFIX%/}"
[ -d "$BUILD_PORT_DIR/src" ] || {
	printf 'committed engine snapshot is incomplete\n' >&2
	exit 1
}
[ "$(git -C "$REPOSITORY" rev-parse HEAD)" = "$PORT_HEAD" ] || {
	printf 'port HEAD changed while freezing one-shot inputs\n' >&2
	exit 1
}
require_clean_port
BUILD_INPUT_MANIFEST="$WORK_DIR/BUILD-CAUSAL-INPUTS.json"
python3 - "$BUILD_PORT_DIR" "$PORT_HEAD" "$SOURCE_DATE_EPOCH" \
	"$BUILD_INPUT_MANIFEST" "${BUILD_RECIPE_PATHS[@]}" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

root = Path(sys.argv[1])
head = sys.argv[2]
epoch = int(sys.argv[3])
output = Path(sys.argv[4])
recipe_paths = tuple(sys.argv[5:])

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
                    raise SystemExit(f"unsafe causal build input: {path}")
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

payload = {
    "causal_inputs": {
        "recipe": {
            **identity(root, recipe_paths),
            "paths": list(recipe_paths),
        },
        "source_tree": identity(root / "src"),
    },
    "port_head": head,
    "port_id": "tearscape",
    "schema": "org.nextos.tearscape.engine-build-inputs",
    "schema_version": 1,
    "source_date_epoch": epoch,
}
encoded = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode()
descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
try:
    view = memoryview(encoded)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise OSError("short write while publishing causal input manifest")
        view = view[written:]
    os.fchmod(descriptor, 0o444)
    os.fsync(descriptor)
finally:
    os.close(descriptor)
PY

# Consume this exact source/recipe/toolchain tuple once, independently of the
# chosen output path or a later documentation-only commit. A failed build is
# still an attempt: its successor must freeze different causal bytes first.
ATTEMPT_CLAIM=$(python3 - \
	"$BUILD_INPUT_MANIFEST" "$FINAL_OUT" \
	"$REPOSITORY" "$NXINPUT_FRAMEWORK_TAG" "$NXINPUT_FRAMEWORK_TAG_OBJECT" \
	"$NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256" "$NXINPUT_FRAMEWORK_COMMIT" \
	"$GODOT_SHA256" "$SDL_SHA256" "$TOOLCHAIN_SHA256" "$SCONS_SHA256" \
	"$SYSROOT_SHA256" "$BUILDER_IMAGE_ID" \
	"$NXINPUT_FRAMEWORK_STATE" "$NXINPUT_COMPONENT_VERSION" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import pwd
import stat
import subprocess
import sys
import ctypes
import errno
import tempfile

manifest_path, final_out = map(Path, sys.argv[1:3])
repository, framework_tag, framework_tag_object, \
    framework_tag_object_sha256, framework_commit = sys.argv[3:8]
pin_names = (
    "godot_sha256", "sdl_sha256", "toolchain_sha256", "scons_sha256",
    "sysroot_sha256", "builder_image_id",
)
pins = dict(zip(pin_names, sys.argv[8:14]))
framework_state, component_version = sys.argv[14:16]
manifest_bytes = manifest_path.read_bytes()
manifest = json.loads(manifest_bytes)
key_material = {
    "causal_inputs": manifest["causal_inputs"],
    "pins": pins,
    "source_date_epoch": manifest["source_date_epoch"],
}
key_bytes = json.dumps(
    key_material, ensure_ascii=False, separators=(",", ":"), sort_keys=True,
).encode()
causal_key = hashlib.sha256(key_bytes).hexdigest()

def git_bytes(*arguments):
    result = subprocess.run(
        ["git", "-C", repository, *arguments], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise OSError(f"cannot resolve immutable nxinput release: {arguments[0]}")
    return result.stdout

def require_released_ref():
    if framework_state == "candidate-commit-pinned":
        # A candidate has NO tag; its identity is the immutable commit object.
        # The window check re-proves the object is still present and still
        # names the pinned component version.
        if git_bytes("cat-file", "-t", framework_commit) != b"commit\n":
            raise OSError("pinned candidate commit is not a commit object")
        version = git_bytes(
            "show", f"{framework_commit}:framework/nxinput/VERSION")
        if version.decode().strip() != component_version:
            raise OSError("pinned candidate commit lost the component VERSION")
        return
    tag_ref = f"refs/tags/{framework_tag}"
    if git_bytes("cat-file", "-t", tag_ref) != b"tag\n":
        raise OSError("nxinput release ref is no longer an annotated tag")
    if git_bytes("rev-parse", tag_ref).strip().decode() != framework_tag_object:
        raise OSError("nxinput release ref moved from the pinned tag object")
    if (git_bytes("rev-parse", f"{tag_ref}^{{commit}}").strip().decode() !=
            framework_commit):
        raise OSError("nxinput release ref moved from the pinned commit")
    payload = git_bytes("cat-file", "tag", tag_ref)
    logical = b"tag " + str(len(payload)).encode() + b"\0" + payload
    if hashlib.sha256(logical).hexdigest() != framework_tag_object_sha256:
        raise OSError("nxinput release ref moved from the pinned SHA-256 object")

require_released_ref()

account_home = Path(pwd.getpwuid(os.geteuid()).pw_dir)
root = account_home
for component in (".local", "state", "nextos-engine-attempts", "tearscape"):
    root = root / component
    try:
        os.mkdir(root, 0o700)
    except FileExistsError:
        pass
    metadata = root.lstat()
    if (not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode) or
            metadata.st_uid != os.geteuid()):
        raise SystemExit("unsafe external engine-attempt ledger")
if stat.S_IMODE(root.lstat().st_mode) != 0o700:
    raise SystemExit("canonical engine-attempt ledger must have mode 0700")
attempt = root / causal_key
try:
    attempt.lstat()
except FileNotFoundError:
    pass
else:
    raise SystemExit(
        "one-shot engine build already consumed for these causal inputs: "
        + str(attempt / "CLAIM.json")
    )
claim = {
    "causal_key": causal_key,
    "final_out": str(final_out),
    "input_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
    "pins": pins,
    "port_head": manifest["port_head"],
    "schema": "org.nextos.tearscape.engine-build-attempt",
    "schema_version": 1,
}
temporary = Path(tempfile.mkdtemp(prefix=f".{causal_key}.", dir=root))
temporary_claim = temporary / "CLAIM.json"
reserved_output = False
published_claim = False

def write_all(descriptor, value):
    view = memoryview(value)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise OSError("short write while publishing one-shot claim")
        view = view[written:]

try:
    parent_metadata = final_out.parent.lstat()
    if stat.S_ISLNK(parent_metadata.st_mode) or not stat.S_ISDIR(parent_metadata.st_mode):
        raise OSError("engine output parent is not a real directory")
    os.mkdir(final_out, 0o500)
    reserved_output = True
    output_metadata = final_out.lstat()
    if (stat.S_ISLNK(output_metadata.st_mode) or
            not stat.S_ISDIR(output_metadata.st_mode) or
            output_metadata.st_uid != os.geteuid() or
            stat.S_IMODE(output_metadata.st_mode) != 0o500):
        raise OSError("engine output reservation is not an owned 0500 directory")
    output_parent_descriptor = os.open(
        final_out.parent, os.O_RDONLY | os.O_DIRECTORY,
    )
    try:
        os.fsync(output_parent_descriptor)
    finally:
        os.close(output_parent_descriptor)

    claim["final_out_reservation"] = {
        "device": output_metadata.st_dev,
        "inode": output_metadata.st_ino,
        "mode": "0500",
    }
    payload = (json.dumps(claim, indent=2, sort_keys=True) + "\n").encode()
    descriptor = os.open(
        temporary_claim,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        0o600,
    )
    try:
        write_all(descriptor, payload)
        os.fchmod(descriptor, 0o444)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.chmod(temporary, 0o500)
    directory_descriptor = os.open(temporary, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_descriptor)
    finally:
        os.close(directory_descriptor)

    # Close the ref-movement window again immediately before the atomic claim.
    require_released_ref()

    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise OSError("renameat2 is required for a no-replace one-shot claim")
    renameat2.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    if renameat2(
            -100, os.fsencode(temporary), -100, os.fsencode(attempt), 1) != 0:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(
                "one-shot engine build already consumed for these causal inputs: "
                + str(attempt / "CLAIM.json")
            )
        raise OSError(error, os.strerror(error), str(attempt))
    published_claim = True
    root_descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(root_descriptor)
    finally:
        os.close(root_descriptor)
except BaseException:
    if not published_claim:
        try:
            os.chmod(temporary, 0o700)
            if temporary_claim.exists():
                temporary_claim.unlink()
            temporary.rmdir()
        except OSError:
            pass
        if reserved_output:
            try:
                final_out.rmdir()
            except OSError:
                pass
    raise
claim_path = attempt / "CLAIM.json"
print(claim_path)
PY
)
printf 'one-shot engine claim: %s\n' "$ATTEMPT_CLAIM"

INPUT_ROOT="$WORK_DIR/inputs"
mkdir -p "$INPUT_ROOT/toolchain" "$INPUT_ROOT/sysroot"
tar -xJf "$DOWNLOAD_DIR/$TOOLCHAIN_ARCHIVE" -C "$INPUT_ROOT/toolchain"
tar -xJf "$DOWNLOAD_DIR/$SYSROOT_ARCHIVE" -C "$INPUT_ROOT/sysroot"
TOOLCHAIN_DIR="$INPUT_ROOT/toolchain/$TOOLCHAIN_NAME"
[ "$("$TOOLCHAIN_DIR/bin/aarch64-none-linux-gnu-g++" -dumpfullversion -dumpversion)" = 10.3.1 ] || {
	printf 'wrong compiler version\n' >&2; exit 1;
}

prepare_source() {
	local destination=$1
	local sdl_source="$destination/.sdl-source"
	mkdir -p "$destination"
	tar -xzf "$DOWNLOAD_DIR/$GODOT_ARCHIVE" --strip-components=1 -C "$destination"
	patch --batch --forward --directory="$destination" -p1 \
		< "$BUILD_PORT_DIR/src/godot_engine/godot-4.6.1-nextos.patch" >/dev/null
	mkdir -p "$sdl_source"
	tar -xzf "$DOWNLOAD_DIR/$SDL_ARCHIVE" --strip-components=1 -C "$sdl_source"
	"$BUILD_PORT_DIR/recipes/vendor_sdl_3_2_30.sh" "$sdl_source" "$destination/thirdparty/sdl"
	rm -rf -- "$sdl_source"
	# Godot's SDL updater applies its disabled-subsystem patch after replacing
	# the vendored source tree. Preserve that order: applying it before the
	# replacement silently restores references to video, tray, async I/O and
	# other SDL implementations that this joystick-only build does not vendor.
	patch --batch --forward --directory="$destination" -p1 \
		< "$BUILD_PORT_DIR/src/godot_engine/sdl-3.2.30-remove-unnecessary-subsystems.patch" >/dev/null
	# 0.2.12 (udev=yes): the re-vendored upstream tree includes <libudev.h>
	# directly, which the offline sysroot does not ship. Godot's own SDL
	# updater fixes every such include (udev, hidapi, dbus, loadso) with its
	# canonical patch, preserved by the vendor step in the Godot checkout.
	patch --batch --forward --directory="$destination" -p1 \
		< "$destination/thirdparty/sdl/patches/0005-fix-libudev-dbus.patch" >/dev/null
	patch --batch --forward --directory="$destination/thirdparty/sdl" -p2 \
		< "$BUILD_PORT_DIR/src/nxinput/sdl3-3.2.30-nxc6-seam.patch" >/dev/null
	install -m 0644 "$BUILD_PORT_DIR"/src/nxinput/*.[ch] \
		"$destination/thirdparty/sdl/joystick/linux/"
	for path in \
		drivers/sdl/SCsub \
		drivers/sdl/joypad_sdl.cpp \
		drivers/sdl/nxinput_gptk_godot.cpp \
		drivers/sdl/nxinput_gptk_godot.h \
		drivers/sdl/tearscape_gptk_context_policy.h \
		drivers/sdl/tearscape_gptk_input_policy.h \
		drivers/sdl/tearscape_gptk_receipt.cpp \
		drivers/sdl/tearscape_gptk_receipt.h \
		drivers/sdl/tearscape_padset.cpp \
		drivers/sdl/tearscape_padset.h \
		platform/linuxbsd/fbdev/display_server_fbdev.cpp \
		platform/linuxbsd/fbdev/display_server_fbdev.h \
		platform/linuxbsd/fbdev/nx_sdl2_geometry.cpp \
		platform/linuxbsd/fbdev/nx_sdl2_geometry.h \
		platform/linuxbsd/fbdev/nx_aspect_policy.h \
		platform/linuxbsd/fbdev/nxcompat_video.c \
		platform/linuxbsd/fbdev/nxcompat_video.h \
		platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.c \
		platform/linuxbsd/fbdev/nxgl_frame_proof_adapter.h \
		platform/linuxbsd/fbdev/nxgl_godot_frame_proof.h; do
		install -m 0644 "$BUILD_PORT_DIR/src/godot_engine/v4-universal/$path" "$destination/$path"
	done
}

run_build() {
	local source_dir=$1 out_dir=$2
	mkdir -p "$out_dir"
	nice -n 10 docker run --rm --network none --read-only --cap-drop ALL \
		--security-opt no-new-privileges --pids-limit 4096 \
		--user "$(id -u):$(id -g)" \
		--tmpfs /tmp:rw,exec,nosuid,nodev,size=2048m \
		-e TEARSCAPE_BUILD_JOBS="$BUILD_JOBS" \
		-e TEARSCAPE_BUILDER_IMAGE_ID="$BUILDER_IMAGE_ID" \
		-e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
		-v "$source_dir:/src:rw" \
		-v "$TOOLCHAIN_DIR:/toolchain:ro" \
		-v "$INPUT_ROOT/sysroot:/buster:ro" \
		-v "$DOWNLOAD_DIR/$SCONS_WHEEL:/deps/$SCONS_WHEEL:ro" \
		-v "$BUILD_PORT_DIR:/port:ro" \
		-v "$BUILD_INPUT_MANIFEST:/build-causal-inputs.json:ro" \
		-v "$out_dir:/out:rw" \
		"$BUILDER_IMAGE" /bin/bash /port/recipes/low_glibc_container.sh
}

SOURCE_ONE="$WORK_DIR/source-one"
OUT_ONE="$WORK_DIR/out-one"
prepare_source "$SOURCE_ONE"
run_build "$SOURCE_ONE" "$OUT_ONE"
# Preserve the un-audited output for post-mortem when the audit refuses it:
# a refused engine must stay inspectable, or the refusal cannot be diagnosed
# without paying a full rebuild. Removed after a fully successful audit.
cp -a -- "$OUT_ONE" "$FINAL_OUT.preaudit"
"$BUILD_PORT_DIR/recipes/audit_low_glibc.sh" \
	"$TOOLCHAIN_DIR/bin/aarch64-none-linux-gnu-" \
	"$OUT_ONE/tearscape-nextos" "$OUT_ONE/shimlib/libEGL.so" \
	"$OUT_ONE/shimlib/libGLESv2.so" | tee "$OUT_ONE/audit.txt"

python3 - "$BUILD_INPUT_MANIFEST" "$OUT_ONE/BUILD-RECEIPT.json" "$OUT_ONE" \
	"$BUILD_JOBS" "$BUILDER_IMAGE_ID" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys

inputs_path, receipt_path, outputs_root = map(Path, sys.argv[1:4])
workers = int(sys.argv[4])
builder_image_id = sys.argv[5]

def load(path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise SystemExit(f"duplicate JSON key in {path}: {key}")
            value[key] = item
        return value
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

metadata = receipt_path.lstat()
if (not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.geteuid() or
        metadata.st_nlink != 1 or stat.S_IMODE(metadata.st_mode) != 0o444):
    raise SystemExit("engine build receipt is not an owned single-link 0444 file")
inputs = load(inputs_path)
receipt = load(receipt_path)
if set(receipt) != {
        "build", "causal_inputs", "outputs", "port_head", "port_id",
        "prepared_source_tree", "schema", "schema_version",
        "source_date_epoch"}:
    raise SystemExit("engine build receipt fields are not canonical")
if (receipt.get("schema") != "org.nextos.tearscape.engine-build-receipt" or
        receipt.get("schema_version") != 1 or receipt.get("port_id") != "tearscape" or
        receipt.get("port_head") != inputs.get("port_head") or
        receipt.get("source_date_epoch") != inputs.get("source_date_epoch") or
        receipt.get("causal_inputs") != inputs.get("causal_inputs")):
    raise SystemExit("engine build receipt differs from its frozen causal inputs")
if receipt.get("build") != {
        "builder_image_id": builder_image_id, "clean_build_count": 1,
        "network": "none", "priority": "nice-10", "workers": workers}:
    raise SystemExit("engine build receipt has the wrong execution boundary")
prepared = receipt.get("prepared_source_tree")
if (not isinstance(prepared, dict) or set(prepared) != {"files", "sha256"} or
        type(prepared.get("files")) is not int or prepared["files"] <= 0 or
        not isinstance(prepared.get("sha256"), str) or
        re.fullmatch(r"[0-9a-f]{64}", prepared["sha256"]) is None):
    raise SystemExit("engine build receipt has no canonical prepared-source identity")
expected_outputs = {}
for relative in ("tearscape-nextos", "shimlib/libEGL.so", "shimlib/libGLESv2.so"):
    path = outputs_root / relative
    expected_outputs[relative] = {"sha256": sha256(path), "size": path.stat().st_size}
if receipt.get("outputs") != expected_outputs:
    raise SystemExit("engine build receipt output identity differs from built bytes")
PY

python3 - "$FINAL_OUT" "$ATTEMPT_CLAIM" <<'PY'
import json
import os
from pathlib import Path
import stat
import sys

output, claim_path = map(Path, sys.argv[1:])
claim_metadata = claim_path.lstat()
attempt_metadata = claim_path.parent.lstat()
if (stat.S_ISLNK(claim_metadata.st_mode) or
        not stat.S_ISREG(claim_metadata.st_mode) or
        claim_metadata.st_uid != os.geteuid() or claim_metadata.st_nlink != 1 or
        stat.S_IMODE(claim_metadata.st_mode) != 0o444 or
        stat.S_ISLNK(attempt_metadata.st_mode) or
        not stat.S_ISDIR(attempt_metadata.st_mode) or
        attempt_metadata.st_uid != os.geteuid() or
        stat.S_IMODE(attempt_metadata.st_mode) != 0o500):
    raise SystemExit("one-shot claim changed during the engine build")
claim = json.loads(claim_path.read_text(encoding="utf-8"))
metadata = output.lstat()
reservation = claim.get("final_out_reservation")
if (set(claim) != {"causal_key", "final_out", "final_out_reservation",
                       "input_manifest_sha256", "pins", "port_head", "schema",
                       "schema_version"} or
        claim.get("schema") != "org.nextos.tearscape.engine-build-attempt" or
        claim.get("schema_version") != 1 or
        claim.get("causal_key") != claim_path.parent.name or
        stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode) or
        metadata.st_uid != os.geteuid() or
        stat.S_IMODE(metadata.st_mode) != 0o500 or any(os.scandir(output)) or
        claim.get("final_out") != str(output) or
        reservation != {"device": metadata.st_dev, "inode": metadata.st_ino,
                        "mode": "0500"}):
    raise SystemExit("reserved engine output changed during the one-shot build")
PY
chmod 0700 "$FINAL_OUT"
mkdir -m 0755 "$FINAL_OUT/shimlib"
install -m 0755 "$OUT_ONE/tearscape-nextos" "$FINAL_OUT/tearscape-nextos"
install -m 0755 "$OUT_ONE/shimlib/libEGL.so" "$FINAL_OUT/shimlib/libEGL.so"
install -m 0755 "$OUT_ONE/shimlib/libGLESv2.so" "$FINAL_OUT/shimlib/libGLESv2.so"
install -m 0644 "$OUT_ONE/audit.txt" "$FINAL_OUT/audit.txt"
install -m 0444 "$OUT_ONE/BUILD-RECEIPT.json" "$FINAL_OUT/BUILD-RECEIPT.json"
python3 - "$FINAL_OUT" "$OUT_ONE" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

root, source = map(Path, sys.argv[1:])

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

root_metadata = root.lstat()
shim_metadata = (root / "shimlib").lstat()
if (stat.S_ISLNK(root_metadata.st_mode) or
        not stat.S_ISDIR(root_metadata.st_mode) or
        root_metadata.st_uid != os.geteuid() or
        stat.S_IMODE(root_metadata.st_mode) != 0o700 or
        stat.S_ISLNK(shim_metadata.st_mode) or
        not stat.S_ISDIR(shim_metadata.st_mode) or
        shim_metadata.st_uid != os.geteuid() or
        stat.S_IMODE(shim_metadata.st_mode) != 0o755 or
        sorted(path.name for path in root.iterdir()) !=
            ["BUILD-RECEIPT.json", "audit.txt", "shimlib", "tearscape-nextos"] or
        sorted(path.name for path in (root / "shimlib").iterdir()) !=
            ["libEGL.so", "libGLESv2.so"]):
    raise SystemExit("engine output publication allowlist or directory modes differ")

files = {
    "tearscape-nextos": (source / "tearscape-nextos", 0o755),
    "shimlib/libEGL.so": (source / "shimlib/libEGL.so", 0o755),
    "shimlib/libGLESv2.so": (source / "shimlib/libGLESv2.so", 0o755),
    "audit.txt": (source / "audit.txt", 0o644),
    "BUILD-RECEIPT.json": (source / "BUILD-RECEIPT.json", 0o444),
}
for relative, (original, mode) in files.items():
    published = root / relative
    metadata = published.lstat()
    original_metadata = original.lstat()
    if (stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode) or
            metadata.st_uid != os.geteuid() or metadata.st_nlink != 1 or
            stat.S_IMODE(metadata.st_mode) != mode or
            stat.S_ISLNK(original_metadata.st_mode) or
            not stat.S_ISREG(original_metadata.st_mode) or
            metadata.st_size != original_metadata.st_size or
            sha256(published) != sha256(original)):
        raise SystemExit(f"published engine output differs from build output: {relative}")
receipt = json.loads((root / "BUILD-RECEIPT.json").read_text(encoding="utf-8"))
for relative in ("tearscape-nextos", "shimlib/libEGL.so", "shimlib/libGLESv2.so"):
    path = root / relative
    if receipt.get("outputs", {}).get(relative) != {
            "sha256": sha256(path), "size": path.stat().st_size}:
        raise SystemExit(f"published engine output differs from receipt: {relative}")

for path in sorted(root.rglob("*"), reverse=True):
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode):
        raise SystemExit(f"refusing symlink in engine output: {path}")
    descriptor = os.open(
        path, os.O_RDONLY | (os.O_DIRECTORY if stat.S_ISDIR(metadata.st_mode) else 0)
    )
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
try:
    os.fsync(descriptor)
finally:
    os.close(descriptor)
os.chmod(root, 0o755)
for directory in (root, root.parent):
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
PY
sha256sum "$FINAL_OUT/tearscape-nextos" "$FINAL_OUT/shimlib/libEGL.so" \
	"$FINAL_OUT/shimlib/libGLESv2.so" "$FINAL_OUT/BUILD-RECEIPT.json"
rm -rf -- "$FINAL_OUT.preaudit"
