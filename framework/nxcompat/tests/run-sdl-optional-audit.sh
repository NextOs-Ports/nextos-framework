#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V4-PRE-02A ELF audit for the optional-SDL resolver.
#
# Host leg (readelf, architecture-independent):
#   - every CLEAN ELF given must carry NO direct dynamic import of
#     SDL_JoystickGetVendor or SDL_JoystickGetProduct (SDL 2.0.6 symbols);
#   - the NEGATIVE control (an ELF that deliberately imports
#     SDL_JoystickGetVendor) must show the import, proving the grep is alive.
#
# nxabi leg (target-architecture, fail-closed):
#   nxabi audits only ARM ELFs, so this script cross-builds the resolver
#   fixture and the negative control for AArch64 and requires
#   `nxabi audit --sdl-floor 2.0.4` to report ZERO sdl-floor findings for the
#   clean fixture and an sdl-floor ERROR for the negative control.  Only the
#   SDL dimension is asserted: host cross toolchains legitimately exceed the
#   public GLIBC ceiling, which the pinned device toolchains satisfy.
#   A missing cross compiler FAILS the gate; it never silently passes.
#
# Usage: run-sdl-optional-audit.sh <clean-elf>... --negative <control-elf>
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
nxcompat_root="$repo_root/framework/nxcompat"
nxabi="$repo_root/framework/nxabi/nxabi.py"
cross_cc=aarch64-linux-gnu-gcc

clean=()
negative=""
while [[ $# -gt 0 ]]; do
  if [[ $1 == --negative ]]; then
    negative=$2
    shift 2
  else
    clean+=("$1")
    shift
  fi
done

if [[ ${#clean[@]} -eq 0 || -z $negative ]]; then
  printf 'usage: %s <clean-elf>... --negative <control-elf>\n' "$0" >&2
  exit 2
fi

work_dir=$(mktemp -d)
cleanup() {
  local status=$?
  trap - EXIT
  rm -rf -- "$work_dir"
  exit "$status"
}
trap cleanup EXIT

direct_imports() {
  readelf -W --dyn-syms "$1" |
    awk '$7 == "UND" {sub(/@.*/, "", $8); print $8}' |
    grep -cxE 'SDL_JoystickGetVendor|SDL_JoystickGetProduct' || true
}

sdl_floor_findings() {
  # nxabi exits nonzero for findings outside the SDL dimension (for example
  # the cross toolchain's glibc ceiling); only the JSON report matters here.
  # A missing or malformed report fails closed in the caller's assignment.
  local report="$work_dir/audit.json"
  rm -f -- "$report"
  python3 -B "$nxabi" audit --sdl-floor 2.0.4 --quiet --json "$report" \
    "$1" >/dev/null 2>&1 || true
  python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    report = json.load(handle)
hits = [f for f in report["findings"] if f["check"] == "sdl-floor"]
for item in hits:
    print("%s: %s" % (item["level"], item["message"]), file=sys.stderr)
print(len(hits))
' "$report"
}

for elf in "${clean[@]}"; do
  test -f "$elf"
  direct=$(direct_imports "$elf")
  if [[ $direct != 0 ]]; then
    printf 'AUDIT FAIL: %s imports SDL_JoystickGetVendor/Product directly\n' \
      "$elf" >&2
    exit 1
  fi
  printf 'readelf ok (no direct 2.0.6 import): %s\n' "$elf"
done

test -f "$negative"
direct=$(direct_imports "$negative")
if [[ $direct == 0 ]]; then
  printf 'AUDIT FAIL: negative control has no direct import; control is dead\n' >&2
  exit 1
fi
printf 'readelf ok (negative control shows the direct import): %s\n' "$negative"

# Static dlopen gate: the production module must resolve strictly inside the
# already-loaded namespace.  Comments and string literals are stripped before
# the scan so prose about dlopen never masks (or fakes) a real call.
production_module="$nxcompat_root/src/nxcompat_sdl_optional.c"
if ! python3 -c '
import re, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    text = handle.read()
text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
text = re.sub(r"//[^\n]*", " ", text)
text = re.sub(r"\"(?:[^\"\\\\]|\\\\.)*\"", "\"\"", text)
bad = re.findall(r"\b(dlopen|dlmopen|dlclose|SDL_LoadObject)\b", text)
if bad:
    print("forbidden call(s) in production module: %s" % sorted(set(bad)),
          file=sys.stderr)
    sys.exit(1)
' "$production_module"; then
  printf 'AUDIT FAIL: %s contains a forbidden loader call\n' \
    "$production_module" >&2
  exit 1
fi
printf 'static ok (no dlopen/dlmopen/dlclose/SDL_LoadObject in the production module)\n'

if ! command -v "$cross_cc" >/dev/null 2>&1; then
  printf 'AUDIT FAIL: %s is required for the nxabi target-architecture leg\n' \
    "$cross_cc" >&2
  exit 1
fi

"$cross_cc" -shared -fPIC -O1 \
  "$nxcompat_root/tests/fixtures/fake_sdl_206.c" \
  -o "$work_dir/libfake-sdl-206.so"
"$cross_cc" -std=c99 -O1 -D_POSIX_C_SOURCE=200809L \
  -DFIXTURE_EXPECT_VENDOR=1 -DFIXTURE_EXPECT_PRODUCT=1 \
  -DFIXTURE_VENDOR_VALUE=0x1209u -DFIXTURE_PRODUCT_VALUE=0x4004u \
  -I"$nxcompat_root/include" \
  "$nxcompat_root/tests/test_sdl_optional_fixture.c" \
  "$nxcompat_root/src/nxcompat_sdl_optional.c" \
  "$work_dir/libfake-sdl-206.so" -ldl \
  -o "$work_dir/audit-fixture-aarch64"
"$cross_cc" -std=c99 -O1 \
  "$nxcompat_root/tests/fixtures/negative_control_direct_import.c" \
  "$work_dir/libfake-sdl-206.so" \
  -o "$work_dir/audit-negative-aarch64"

fixture_direct=$(direct_imports "$work_dir/audit-fixture-aarch64")
if [[ $fixture_direct != 0 ]]; then
  printf 'AUDIT FAIL: aarch64 fixture imports the 2.0.6 symbols directly\n' >&2
  exit 1
fi
# ELF-level dlopen gate on a sanitizer-free build of the production module:
# only dlsym/dladdr may be imported, never dlopen/dlmopen/dlclose.
loader_imports=$(readelf -W --dyn-syms "$work_dir/audit-fixture-aarch64" |
  awk '$7 == "UND" {sub(/@.*/, "", $8); print $8}' |
  grep -cxE 'dlopen|dlmopen|dlclose' || true)
if [[ $loader_imports != 0 ]]; then
  printf 'AUDIT FAIL: aarch64 fixture ELF imports dlopen/dlmopen/dlclose\n' >&2
  exit 1
fi
printf 'static ok (aarch64 fixture ELF imports no dlopen/dlmopen/dlclose)\n'
floor_hits=$(sdl_floor_findings "$work_dir/audit-fixture-aarch64" | tail -n1)
if [[ $floor_hits != 0 ]]; then
  printf 'AUDIT FAIL: aarch64 resolver fixture violates the SDL 2.0.4 floor\n' >&2
  exit 1
fi
printf 'nxabi ok (aarch64 resolver fixture clean at floor 2.0.4)\n'

floor_hits=$(sdl_floor_findings "$work_dir/audit-negative-aarch64" | tail -n1)
if [[ $floor_hits == 0 || -z $floor_hits ]]; then
  printf 'AUDIT FAIL: nxabi did not flag the direct 2.0.6 import; the floor gate is blind\n' >&2
  exit 1
fi
printf 'nxabi ok (aarch64 negative control flagged by sdl-floor)\n'
printf 'nxcompat-sdl-optional-audit: PASS\n'
