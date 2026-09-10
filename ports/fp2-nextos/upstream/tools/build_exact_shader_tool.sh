#!/usr/bin/env bash
# Build the host-only Unity SMOL-V/SPIR-V -> ESSL100 translator.
#
# Usage:
#   build_exact_shader_tool.sh SPIRV_CROSS_SOURCE SMOLV_SOURCE [OUTPUT]
#
# SPIRV-Cross is consumed under its Apache-2.0 license and SMOL-V is public
# domain.  Neither dependency nor this host executable is shipped in the port.
set -euo pipefail

TOOL_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$TOOL_DIR/.." && pwd -P)
SPIRV_CROSS_SRC=${1:-}
SMOLV_SRC=${2:-}
OUTPUT=${3:-$PORT_DIR/build/host/unity-smolv-cross}

SPIRV_CROSS_COMMIT=eb32b288ea553e938005fcfd819a2290b1c8032d
SPIRV_TOOLS_VERSION=2026.3.1
SMOLV_CPP_SHA256=d6fae91910dda8d0a5f9cc863829bb686815e0fab57235f78cd24ef2f75d72f1
SMOLV_H_SHA256=679e0eb13942d84ef56f1924d239eb93ab3d072a443b968097dd445aa51907e1

fail() {
  printf 'fp2 exact-shader build error: %s\n' "$*" >&2
  exit 1
}

[[ -n $SPIRV_CROSS_SRC && -n $SMOLV_SRC ]] ||
  fail "usage: $0 SPIRV_CROSS_SOURCE SMOLV_SOURCE [OUTPUT]"
[[ -d $SPIRV_CROSS_SRC && -d $SMOLV_SRC ]] ||
  fail "dependency source directory is missing"

for tool in g++ git pkg-config sha256sum install; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

actual_cross_commit=$(git -C "$SPIRV_CROSS_SRC" rev-parse HEAD 2>/dev/null) ||
  fail "SPIRV_CROSS_SOURCE is not a Git checkout"
[[ $actual_cross_commit == "$SPIRV_CROSS_COMMIT" ]] ||
  fail "SPIRV-Cross commit is $actual_cross_commit, expected $SPIRV_CROSS_COMMIT"

actual_spirv_tools=$(pkg-config --modversion SPIRV-Tools 2>/dev/null) ||
  fail "SPIRV-Tools development package is unavailable"
[[ $actual_spirv_tools == "$SPIRV_TOOLS_VERSION" ]] ||
  fail "SPIRV-Tools is $actual_spirv_tools, expected $SPIRV_TOOLS_VERSION"

actual_smolv_cpp=$(sha256sum "$SMOLV_SRC/smolv.cpp" | awk '{print $1}')
actual_smolv_h=$(sha256sum "$SMOLV_SRC/smolv.h" | awk '{print $1}')
[[ $actual_smolv_cpp == "$SMOLV_CPP_SHA256" ]] ||
  fail "smolv.cpp does not match the pinned source"
[[ $actual_smolv_h == "$SMOLV_H_SHA256" ]] ||
  fail "smolv.h does not match the pinned source"

for source in spirv_cfg.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp \
              spirv_parser.cpp spirv_glsl.cpp; do
  [[ -f $SPIRV_CROSS_SRC/$source ]] || fail "missing SPIRV-Cross source: $source"
done

BUILD_DIR=$(mktemp -d)
trap 'rm -rf -- "$BUILD_DIR"' EXIT INT TERM
export LC_ALL=C

printf 'fp2 exact-shader: building pinned host translator\n'
printf '  SPIRV-Cross %s\n' "$SPIRV_CROSS_COMMIT"
printf '  SPIRV-Tools %s\n' "$SPIRV_TOOLS_VERSION"

g++ -std=c++17 -O2 \
  -I"$SPIRV_CROSS_SRC" -I"$SMOLV_SRC" \
  "$TOOL_DIR/unity_smolv_cross.cpp" \
  "$SMOLV_SRC/smolv.cpp" \
  "$SPIRV_CROSS_SRC/spirv_cfg.cpp" \
  "$SPIRV_CROSS_SRC/spirv_cross.cpp" \
  "$SPIRV_CROSS_SRC/spirv_cross_parsed_ir.cpp" \
  "$SPIRV_CROSS_SRC/spirv_parser.cpp" \
  "$SPIRV_CROSS_SRC/spirv_glsl.cpp" \
  -lSPIRV-Tools-opt -lSPIRV-Tools \
  -Wl,-z,relro,-z,now \
  -o "$BUILD_DIR/unity-smolv-cross"

install -D -m 0755 "$BUILD_DIR/unity-smolv-cross" "$OUTPUT"
printf 'fp2 exact-shader: built %s\n' "$OUTPUT"
sha256sum "$OUTPUT"
