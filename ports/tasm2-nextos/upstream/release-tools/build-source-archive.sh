#!/usr/bin/env bash
# Build the deterministic corresponding-source archive for the ASM2 port.
#
# This script never downloads dependencies. All repositories must already
# contain the exact commits named below, so an archive can be reproduced
# offline after the inputs are acquired.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

BOX64_COMMIT=3ec5de03c786333ed8d5a51c5b35a8bd6e22b229
SDL2_COMPAT_COMMIT=3e1fa90d301428ace65d5c8b371e93d2c59c3d65
NXEXTRACT_COMMIT=8229cc857467e8641e7e2d6021c6a9bdea63afbf
NXEXTRACT_UPSTREAM_PY_SHA256=32cd5ed702ba2a0abfe1b63cb086e442675178d4beffccfdf6dc31940db9bacb
NXEXTRACT_DOWNSTREAM_PY_SHA256=fc21745f059a926a67b2d5ee7217dc680118adcc90d015d8d74f3445d81178f4
GCC_EXCEPTION_SHA256=9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74

VERSION=1.1.8
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}
PORT_SOURCE=
PACKAGE_SOURCE=
BOX64_REPO=
SDL2_COMPAT_REPO=
NXEXTRACT_REPO=
PORT_REVISION=
OUTPUT=
RELEASE_MODE=0
BOX64_PATCH=
BOX64_PATCH_EXPECTED=UNFROZEN
I386_LOADER_SHA256=UNFROZEN
BOX32_RUNTIME_SHA256=UNFROZEN

fail() {
  printf 'source archive error: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
usage: build-source-archive.sh [options]

Required:
  --port-source DIR         exact ASM2 port source snapshot
  --package-source DIR      universal package-src directory
  --box64-repo DIR          Box64 git checkout containing the pinned commit
  --sdl2-compat-repo DIR    sdl2-compat git checkout containing the pinned commit
  --nxextract-repo DIR      NXExtract git checkout containing the pinned commit
  --port-revision VALUE     port source revision recorded in provenance
  --output FILE.tar.gz      output archive

Optional:
  --box64-patch FILE        GPU/SDL patch (default: adjacent draft patch)
  --box64-patch-sha256 HASH expected hash (mandatory with --release)
  --i386-loader-sha256 HASH packaged i386 loader hash (mandatory with --release)
  --box32-runtime-sha256 HASH packaged Box32 hash (mandatory with --release)
  --version VERSION         source package version (default: 1.1.8)
  --release                 require an immutable clean 40-hex port revision
  --help
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --port-source)
      [ "$#" -ge 2 ] || fail "--port-source requires a value"
      PORT_SOURCE=$2
      shift 2
      ;;
    --package-source)
      [ "$#" -ge 2 ] || fail "--package-source requires a value"
      PACKAGE_SOURCE=$2
      shift 2
      ;;
    --box64-repo)
      [ "$#" -ge 2 ] || fail "--box64-repo requires a value"
      BOX64_REPO=$2
      shift 2
      ;;
    --sdl2-compat-repo)
      [ "$#" -ge 2 ] || fail "--sdl2-compat-repo requires a value"
      SDL2_COMPAT_REPO=$2
      shift 2
      ;;
    --nxextract-repo)
      [ "$#" -ge 2 ] || fail "--nxextract-repo requires a value"
      NXEXTRACT_REPO=$2
      shift 2
      ;;
    --box64-patch)
      [ "$#" -ge 2 ] || fail "--box64-patch requires a value"
      BOX64_PATCH=$2
      shift 2
      ;;
    --box64-patch-sha256)
      [ "$#" -ge 2 ] || fail "--box64-patch-sha256 requires a value"
      BOX64_PATCH_EXPECTED=$2
      shift 2
      ;;
    --i386-loader-sha256)
      [ "$#" -ge 2 ] || fail "--i386-loader-sha256 requires a value"
      I386_LOADER_SHA256=$2
      shift 2
      ;;
    --box32-runtime-sha256)
      [ "$#" -ge 2 ] || fail "--box32-runtime-sha256 requires a value"
      BOX32_RUNTIME_SHA256=$2
      shift 2
      ;;
    --port-revision)
      [ "$#" -ge 2 ] || fail "--port-revision requires a value"
      PORT_REVISION=$2
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || fail "--output requires a value"
      OUTPUT=$2
      shift 2
      ;;
    --version)
      [ "$#" -ge 2 ] || fail "--version requires a value"
      VERSION=$2
      shift 2
      ;;
    --release)
      RELEASE_MODE=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

for required in \
  PORT_SOURCE PACKAGE_SOURCE BOX64_REPO SDL2_COMPAT_REPO NXEXTRACT_REPO \
  PORT_REVISION OUTPUT; do
  [ -n "${!required}" ] || fail "missing required option for $required"
done

case "$VERSION" in
  ''|*[!A-Za-z0-9._-]*) fail "unsafe version: $VERSION" ;;
esac
case "$SOURCE_DATE_EPOCH" in
  ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH must be an integer" ;;
esac
[ "$SOURCE_DATE_EPOCH" -ge 315532800 ] ||
  fail "SOURCE_DATE_EPOCH predates portable archive timestamps"

if [ "$RELEASE_MODE" -eq 1 ]; then
  [[ "$PORT_REVISION" =~ ^[0-9a-f]{40}$ ]] ||
    fail "release port revision must be a full 40-hex commit"
  [[ "$BOX64_PATCH_EXPECTED" =~ ^[0-9a-f]{64}$ ]] ||
    fail "release Box64 patch hash must be a full 64-hex SHA-256"
  [[ "$I386_LOADER_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    fail "release i386 loader hash must be a full 64-hex SHA-256"
  [[ "$BOX32_RUNTIME_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    fail "release Box32 runtime hash must be a full 64-hex SHA-256"
  [ "$I386_LOADER_SHA256" != \
    fd6b48f4d89b1b9ff67af2cb34a67a4e61d7259eed34e52c781ff91a85481b8d ] ||
    fail "release source archive refuses the rejected fd6 i386 loader"
fi

for tool in awk basename bash cat chmod cmp diff dirname find git grep gzip \
            install mkdir mktemp patch python3 readlink rm sed sha256sum sort \
            tar touch; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

canonical_dir() {
  [ -d "$1" ] && [ ! -L "$1" ] ||
    fail "source directory is missing or linked: $1"
  (cd -- "$1" && pwd -P)
}

PORT_SOURCE=$(canonical_dir "$PORT_SOURCE")
PACKAGE_SOURCE=$(canonical_dir "$PACKAGE_SOURCE")
BOX64_REPO=$(canonical_dir "$BOX64_REPO")
SDL2_COMPAT_REPO=$(canonical_dir "$SDL2_COMPAT_REPO")
NXEXTRACT_REPO=$(canonical_dir "$NXEXTRACT_REPO")

if [ "$RELEASE_MODE" -eq 1 ]; then
  git -C "$PORT_SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "release port source must be an immutable git worktree"
  [ "$(git -C "$PORT_SOURCE" rev-parse HEAD)" = "$PORT_REVISION" ] ||
    fail "release port revision does not match the source worktree"
  [ -z "$(git -C "$PORT_SOURCE" status --short -- .)" ] ||
    fail "release port source contains uncommitted files"
fi

case "$OUTPUT" in
  *.tar.gz) ;;
  *) fail "output must end in .tar.gz" ;;
esac

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
[ -n "$BOX64_PATCH" ] ||
  BOX64_PATCH="$SCRIPT_DIR/box64-x5m-s7d.patch"
GCC_EXCEPTION="$SCRIPT_DIR/../licenses/GCC-Runtime-Library-Exception-3.1.txt"

BOX64_PATCH=$(readlink -f -- "$BOX64_PATCH") ||
  fail "could not resolve Box64 patch"
[ -f "$BOX64_PATCH" ] && [ ! -L "$BOX64_PATCH" ] ||
  fail "Box64 patch is missing or linked"
BOX64_PATCH_SHA256=$(sha256sum "$BOX64_PATCH" | awk '{print $1}')
if [ "$BOX64_PATCH_EXPECTED" != UNFROZEN ]; then
  [ "$BOX64_PATCH_SHA256" = "$BOX64_PATCH_EXPECTED" ] ||
    fail "Box64 patch does not match the requested SHA-256"
fi
[ "$(sha256sum "$GCC_EXCEPTION" | awk '{print $1}')" = \
  "$GCC_EXCEPTION_SHA256" ] || fail "GCC Runtime Library Exception changed"

for required_patch_path in \
  src/custommmap.c \
  src/tools/box64stack.c \
  src/wrapped32/wrappedsdl2_private.h; do
  grep -Fq "diff --git a/$required_patch_path b/$required_patch_path" \
    "$BOX64_PATCH" ||
    fail "Box64 patch does not cover $required_patch_path"
done
if sed -n 's#^diff --git a/[^ ]* b/\\([^ ]*\\)$#\\1#p' "$BOX64_PATCH" |
   grep -Ev \
     '^(src/custommmap[.]c|src/tools/box64stack[.]c|src/wrapped32/wrappedsdl2_private[.]h)$' |
   grep . >/dev/null; then
  fail "Box64 patch modifies a file outside the audited GPU/SDL wrapper scope"
fi

git -C "$BOX64_REPO" cat-file -e "$BOX64_COMMIT^{commit}" ||
  fail "Box64 pinned commit is unavailable"

git -C "$SDL2_COMPAT_REPO" cat-file -e "$SDL2_COMPAT_COMMIT^{commit}" ||
  fail "sdl2-compat pinned commit is unavailable"
git -C "$NXEXTRACT_REPO" cat-file -e "$NXEXTRACT_COMMIT^{commit}" ||
  fail "NXExtract pinned commit is unavailable"

[ -f "$PORT_SOURCE/src/main.c" ] ||
  fail "port source does not contain src/main.c"
[ -f "$PORT_SOURCE/build_buster_x86_box32.sh" ] ||
  fail "port source does not contain the public low-glibc i386 build recipe"
[ -f "$PACKAGE_SOURCE/build-package.sh" ] &&
  [ -f "$PACKAGE_SOURCE/sources/nxextract.py" ] ||
  fail "package source layout is incomplete"
[ "$(git -C "$NXEXTRACT_REPO" show \
  "$NXEXTRACT_COMMIT:nxextract.py" | sha256sum | awk '{print $1}')" = \
  "$NXEXTRACT_UPSTREAM_PY_SHA256" ] ||
  fail "NXExtract upstream Python source changed"
[ "$(sha256sum "$PACKAGE_SOURCE/sources/nxextract.py" | awk '{print $1}')" = \
  "$NXEXTRACT_DOWNSTREAM_PY_SHA256" ] ||
  fail "packaged NXExtract downstream source changed without provenance"

if [ "$RELEASE_MODE" -eq 1 ]; then
  [ -f "$PACKAGE_SOURCE/sources/asm2_127_x86_box32" ] &&
    [ ! -L "$PACKAGE_SOURCE/sources/asm2_127_x86_box32" ] ||
    fail "release package source has no regular i386 loader"
  [ "$(sha256sum "$PACKAGE_SOURCE/sources/asm2_127_x86_box32" |
    awk '{print $1}')" = "$I386_LOADER_SHA256" ] ||
    fail "release package i386 loader differs from source provenance"
  [ -f "$PACKAGE_SOURCE/sources/runtime/x5m/box64" ] &&
    [ ! -L "$PACKAGE_SOURCE/sources/runtime/x5m/box64" ] ||
    fail "release package source has no regular Box32 runtime"
  [ "$(sha256sum "$PACKAGE_SOURCE/sources/runtime/x5m/box64" |
    awk '{print $1}')" = "$BOX32_RUNTIME_SHA256" ] ||
    fail "release package Box32 differs from source provenance"

  python3 - \
    "$PACKAGE_SOURCE/sources/BUILD-PROVENANCE.json" \
    "$PACKAGE_SOURCE/sources/run.sh" \
    "$PACKAGE_SOURCE/build-package.sh" \
    "$I386_LOADER_SHA256" \
    "$BOX32_RUNTIME_SHA256" <<'PY'
import json
from pathlib import Path
import sys

provenance_path, launcher_path, builder_path, i386_hash, box32_hash = sys.argv[1:]
with open(provenance_path, encoding="utf-8") as stream:
    provenance = json.load(stream)

loaders = provenance.get("loaders", {})
if loaders.get("asm2_127_x86_box32", {}).get("sha256") != i386_hash:
    raise SystemExit("release build provenance does not freeze the i386 loader")
x5 = provenance.get("x5m_runtime", {})
if x5.get("runtime/x5m/box64", {}).get("sha256") != box32_hash:
    raise SystemExit("release build provenance does not freeze Box32")
validation = x5.get("validation", {})
if validation.get("component_bytes_frozen") is not True:
    raise SystemExit("release build provenance leaves X5M components unfrozen")

launcher = Path(launcher_path).read_text(encoding="utf-8")
if f"X5_GUEST_BIN_SHA256={i386_hash}" not in launcher:
    raise SystemExit("release launcher does not pin the i386 loader")
if "__ASM2_" in launcher:
    raise SystemExit("release launcher retains a hash gate token")
if "BOX64_DYNAREC_EAGER=" in launcher:
    raise SystemExit("release launcher uses experimental eager mode")
function_start = launcher.find("configure_x5m_runtime() {")
function_end = launcher.find("\n}", function_start)
if function_start < 0 or function_end < 0:
    raise SystemExit("release launcher has no bounded X5M runtime function")
x5_function = launcher[function_start:function_end]
for assignment in (
    "export BOX64_DYNAREC=1",
    "export BOX64_DYNAREC_BIGBLOCK=0",
    "export BOX64_DYNAREC_SAFEFLAGS=2",
):
    if launcher.count(assignment) != 1 or assignment not in x5_function:
        raise SystemExit(
            f"release launcher lacks a uniquely scoped dynarec safety assignment: {assignment}"
        )

builder = Path(builder_path).read_text(encoding="utf-8")
if "X5M_RELEASE_DEVICE_GATE_PASSED=1" not in builder:
    raise SystemExit("package builder still has the real-device gate closed")
if f"X86_RELEASE_SHA256={i386_hash}" not in builder:
    raise SystemExit("package builder does not pin the release i386 hash")
if f"BOX32_RELEASE_SHA256={box32_hash}" not in builder:
    raise SystemExit("package builder does not pin the release Box32 hash")
PY
fi

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/asm2-source-archive.XXXXXX")
ROOT_NAME="asm2-port-source-$VERSION"
STAGE="$TMP_ROOT/$ROOT_NAME"
TMP_ARCHIVE="$TMP_ROOT/$ROOT_NAME.tar.gz"

cleanup() {
  rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM
mkdir -p -- "$STAGE"

put() {
  local mode=$1 source=$2 destination=$3

  [ -f "$source" ] && [ ! -L "$source" ] ||
    fail "source file is missing or linked: $source"
  install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

copy_port_sources() {
  local path relative mode

  while IFS= read -r -d '' path; do
    relative=${path#"$PORT_SOURCE/"}
    case "$relative" in
      src/*.c|src/*.h|src/*.S|\
      tests/*.c|tests/*.h|tests/*.sh|tests/*.py|\
      tools/*.py|tools/*.sh)
        ;;
      *) continue ;;
    esac
    mode=0644
    case "$relative" in
      *.sh|*.py) mode=0755 ;;
    esac
    put "$mode" "$path" "asm2_127/$relative"
  done < <(find "$PORT_SOURCE/src" "$PORT_SOURCE/tests" "$PORT_SOURCE/tools" \
    -type f -print0 | sort -z)

  for relative in \
    .gitignore \
    CLEANROOM.md \
    README.md \
    "The Amazing Spider-Man 2 (Multi).sh" \
    "The Amazing Spider-Man 2.sh" \
    build.sh \
    build_buster_arkos.sh \
    build_buster_x86_box32.sh \
    build_x86_box32.sh \
    build_x86_smoke.sh \
    x5m-runtime-env.sh; do
    [ -f "$PORT_SOURCE/$relative" ] || continue
    mode=0644
    case "$relative" in *.sh) mode=0755 ;; esac
    put "$mode" "$PORT_SOURCE/$relative" "asm2_127/$relative"
  done
}

copy_package_sources() {
  local path relative mode

  put 0755 "$PACKAGE_SOURCE/build-package.sh" \
    "package/build-package.sh"
  put 0644 "$PACKAGE_SOURCE/package-files.txt" \
    "package/package-files.txt"

  while IFS= read -r -d '' path; do
    relative=${path#"$PACKAGE_SOURCE/sources/"}
    case "$relative" in
      bin/*|cover.png|screenshot.png|nxextract-ui|*/__pycache__/*)
        continue
        ;;
      *.sh|*.py|*.json|*.md|*.txt|*.xml)
        ;;
      *) continue ;;
    esac
    mode=0644
    case "$relative" in *.sh|*.py) mode=0755 ;; esac
    put "$mode" "$path" "package/sources/$relative"
  done < <(find "$PACKAGE_SOURCE/sources" -type f -print0 | sort -z)
}

copy_port_sources
copy_package_sources

put 0755 "$SCRIPT_DIR/build-source-archive.sh" \
  "release-tools/build-source-archive.sh"
put 0755 "$SCRIPT_DIR/build-box32-x5m.sh" \
  "release-tools/build-box32-x5m.sh"
put 0755 "$SCRIPT_DIR/build-sdl2-compat-x5m.sh" \
  "release-tools/build-sdl2-compat-x5m.sh"
put 0755 "$SCRIPT_DIR/audit-public-package.sh" \
  "release-tools/audit-public-package.sh"
put 0755 "$SCRIPT_DIR/launcher-x5m-test.sh" \
  "release-tools/launcher-x5m-test.sh"
put 0644 "$SCRIPT_DIR/ART-PROVENANCE.example.json" \
  "release-tools/ART-PROVENANCE.example.json"
put 0644 "$SCRIPT_DIR/SOURCE-ARCHIVE-README.md" "README.md"
put 0644 "$BOX64_PATCH" \
  "patches/box64-x5m-s7d.patch"
put 0644 "$GCC_EXCEPTION" \
  "licenses/GCC-Runtime-Library-Exception-3.1.txt"
put 0644 "$PACKAGE_SOURCE/sources/licenses/GPL-3.0.txt" \
  "licenses/GPL-3.0.txt"

mkdir -p "$STAGE/third_party/box64"
git -C "$BOX64_REPO" archive "$BOX64_COMMIT" |
  tar -xf - -C "$STAGE/third_party/box64"
patch -s -d "$STAGE/third_party/box64" -p1 < "$BOX64_PATCH"

mkdir -p "$STAGE/third_party/sdl2-compat"
git -C "$SDL2_COMPAT_REPO" archive "$SDL2_COMPAT_COMMIT" |
  tar -xf - -C "$STAGE/third_party/sdl2-compat"

mkdir -p "$STAGE/third_party/nxextract"
git -C "$NXEXTRACT_REPO" archive "$NXEXTRACT_COMMIT" |
  tar -xf - -C "$STAGE/third_party/nxextract"
put 0755 "$PACKAGE_SOURCE/sources/nxextract.py" \
  "third_party/nxextract/downstream/nxextract.py"
set +e
diff -u \
  --label a/nxextract.py \
  --label b/nxextract.py \
  "$STAGE/third_party/nxextract/nxextract.py" \
  "$STAGE/third_party/nxextract/downstream/nxextract.py" \
  > "$STAGE/patches/nxextract-1.1.2-asm2.1.patch"
diff_status=$?
set -e
[ "$diff_status" -eq 1 ] ||
  fail "NXExtract downstream patch generation returned $diff_status"

# The upstream Box64 repository tracks prebuilt test/runtime ELFs and shared
# library symlinks that are not needed to rebuild the scoped host. Remove those
# artifacts while preserving every source file, and record the exclusions.
python3 - "$STAGE" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
removed = []
for path in sorted((root / "third_party" / "box64").rglob("*")):
    if path.is_symlink():
        removed.append(path.relative_to(root).as_posix() + " [symlink]")
        path.unlink()
        continue
    if not path.is_file():
        continue
    try:
        with path.open("rb") as stream:
            magic = stream.read(4)
    except OSError:
        continue
    if magic == b"\x7fELF" or path.suffix in {".a", ".o", ".so"}:
        removed.append(path.relative_to(root).as_posix())
        path.unlink()

report = (
    root / "third_party" / "box64" /
    "REMOVED-UPSTREAM-BINARY-ARTIFACTS.txt"
)
report.write_text(
    "Prebuilt upstream binary/symlink artifacts omitted from this "
    "source-only archive:\n"
    + "".join(f"{entry}\n" for entry in removed),
    encoding="utf-8",
)
PY

tree_digest() {
  local directory=$1

  (
    cd -- "$directory"
    while IFS= read -r -d '' relative; do
      sha256sum -- "${relative#./}"
    done < <(find . -type f -print0 | sort -z)
  ) | sha256sum | awk '{print $1}'
}

PORT_TREE_SHA256=$(tree_digest "$STAGE/asm2_127")
PACKAGE_TREE_SHA256=$(tree_digest "$STAGE/package")
BOX64_TREE_SHA256=$(tree_digest "$STAGE/third_party/box64")
SDL2_TREE_SHA256=$(tree_digest "$STAGE/third_party/sdl2-compat")
NXEXTRACT_TREE_SHA256=$(tree_digest "$STAGE/third_party/nxextract")
NXEXTRACT_PATCH_SHA256=$(sha256sum \
  "$STAGE/patches/nxextract-1.1.2-asm2.1.patch" | awk '{print $1}')

python3 - \
  "$STAGE/SOURCE-PROVENANCE.json" \
  "$VERSION" \
  "$SOURCE_DATE_EPOCH" \
  "$PORT_REVISION" \
  "$PORT_TREE_SHA256" \
  "$PACKAGE_TREE_SHA256" \
  "$BOX64_COMMIT" \
  "$BOX64_PATCH_SHA256" \
  "$BOX64_TREE_SHA256" \
  "$SDL2_COMPAT_COMMIT" \
  "$SDL2_TREE_SHA256" \
  "$NXEXTRACT_COMMIT" \
  "$NXEXTRACT_PATCH_SHA256" \
  "$NXEXTRACT_TREE_SHA256" \
  "$GCC_EXCEPTION_SHA256" \
  "$I386_LOADER_SHA256" \
  "$BOX32_RUNTIME_SHA256" <<'PY'
import json
from pathlib import Path
import sys

(
    destination,
    version,
    epoch,
    port_revision,
    port_tree,
    package_tree,
    box_commit,
    box_patch,
    box_tree,
    sdl_commit,
    sdl_tree,
    nx_commit,
    nx_patch,
    nx_tree,
    gcc_exception,
    i386_loader,
    box32_runtime,
) = sys.argv[1:]

document = {
    "schema": 1,
    "source_package_version": version,
    "source_date_epoch": int(epoch),
    "asm2": {
        "port_revision": port_revision,
        "port_source_tree_sha256": port_tree,
        "package_source_tree_sha256": package_tree,
        "i386_loader_binary_sha256": i386_loader,
    },
    "box64_box32": {
        "upstream": "https://github.com/ptitSeb/box64",
        "upstream_commit": box_commit,
        "downstream_patch_sha256": box_patch,
        "patched_source_tree_sha256": box_tree,
        "runtime_binary_sha256": box32_runtime,
        "runtime_policy": {
            "BOX64_DYNAREC": "1",
            "BOX64_DYNAREC_BIGBLOCK": "0",
            "BOX64_DYNAREC_SAFEFLAGS": "2",
            "scope": "configure_x5m_runtime only",
        },
        "license": "MIT",
    },
    "sdl2_compat": {
        "upstream": "https://github.com/libsdl-org/sdl2-compat",
        "upstream_commit": sdl_commit,
        "source_tree_sha256": sdl_tree,
        "version": "2.32.71-development",
        "license": "Zlib",
    },
    "nxextract": {
        "upstream": "https://github.com/NextOs-Ports/NXExtract",
        "upstream_commit": nx_commit,
        "downstream_revision": "1.1.2-asm2.1",
        "downstream_patch_sha256": nx_patch,
        "source_tree_sha256": nx_tree,
        "license": "MIT",
    },
    "gcc_runtime": {
        "linkage": "static-libgcc",
        "runtime_library_exception": "3.1",
        "exception_sha256": gcc_exception,
    },
}
Path(destination).write_text(
    json.dumps(document, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY

# Reject game/runtime data, local build artifacts and private test context.
if find "$STAGE" \( \
    -type l -o \
    -name '*.apk' -o -name '*.apks' -o -name '*.apkm' -o \
    -name '*.xapk' -o -name '*.obb' -o -name '*.dex' -o \
    -name 'libtasm2.so' -o -name 'libtasm2-x86.so' -o \
    -name '*.o' -o -name '*.a' -o -name '*.so' -o \
    -name '*.log' -o -name '__pycache__' -o -name '*.pyc' -o \
    -name 'debug.log' \
  \) -print -quit | grep . >/dev/null; then
  find "$STAGE" \( \
    -type l -o \
    -name '*.apk' -o -name '*.obb' -o -name '*.dex' -o \
    -name 'libtasm2.so' -o -name 'libtasm2-x86.so' -o \
    -name '*.o' -o -name '*.a' -o -name '*.so' -o \
    -name '*.log' -o -name '__pycache__' -o -name '*.pyc' \
  \) -print >&2
  fail "binary, proprietary, linked or generated data entered source staging"
fi

python3 - "$STAGE" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
for path in root.rglob("*"):
    if path.is_file():
        with path.open("rb") as stream:
            if stream.read(4) == b"\x7fELF":
                raise SystemExit(f"ELF entered source-only archive: {path.relative_to(root)}")
PY

python3 - "$STAGE" "$RELEASE_MODE" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
release_mode = sys.argv[2] == "1"
needles = {
    b"private IPv4 prefix": b"192" + b".168" + b".",
    b"personal home": b"/home/" + b"felipe",
    b"internal archive": b"/mnt/" + b"ARQUIVOS",
    b"agent temporary path": b"/tmp/" + b"claude",
    b"test root login": b"root" + b"@",
    b"test ark login": b"ark" + b"@",
}
findings = []
for path in sorted(root.rglob("*")):
    if not path.is_file() or path.is_symlink():
        continue
    payload = path.read_bytes()
    for label, needle in needles.items():
        if needle in payload:
            relative = path.relative_to(root).as_posix()
            # The package builder contains these tokens solely inside its own
            # privacy gate. They are signatures, not leaked runtime context.
            if relative == "package/build-package.sh":
                continue
            findings.append(
                f"{relative}: {label.decode()}"
            )
if findings:
    message = (
        "personal path, test address or credential marker entered source "
        "archive:\n" + "\n".join(findings)
    )
    if release_mode:
        raise SystemExit(message)
    (root / "DRAFT-AUDIT-WARNINGS.txt").write_text(
        "NON-RELEASE SOURCE SNAPSHOT\n" + message + "\n",
        encoding="utf-8",
    )
    print(message, file=sys.stderr)
PY

(
  cd -- "$STAGE"
  while IFS= read -r -d '' relative; do
    relative=${relative#./}
    [ "$relative" = SHA256SUMS ] && continue
    sha256sum -- "$relative"
  done < <(find . -type f -print0 | sort -z)
) > "$STAGE/SHA256SUMS"
(
  cd -- "$STAGE"
  sha256sum -c SHA256SUMS >/dev/null
)

find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
find "$STAGE" -type f \( -name '*.sh' -o -name '*.py' \) \
  -exec chmod 0755 {} +
find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

(
  cd -- "$TMP_ROOT"
  tar \
    --sort=name \
    --format=gnu \
    --mtime="@$SOURCE_DATE_EPOCH" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -cf - "$ROOT_NAME" |
    gzip -n -9 > "$TMP_ARCHIVE"
)
gzip -t "$TMP_ARCHIVE"

OUTPUT_DIR=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTPUT_DIR"
OUTPUT_DIR=$(cd -- "$OUTPUT_DIR" && pwd -P)
OUTPUT="$OUTPUT_DIR/$(basename -- "$OUTPUT")"
install -m 0644 -- "$TMP_ARCHIVE" "$OUTPUT"
(
  cd -- "$OUTPUT_DIR"
  sha256sum -- "$(basename -- "$OUTPUT")" \
    > "$(basename -- "$OUTPUT").sha256"
)

printf 'OK: %s\n' "$OUTPUT"
printf 'Source archive: '
sha256sum "$OUTPUT"
