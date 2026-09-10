#!/usr/bin/env bash
# M11 ARMv7 lifecycle gate: GCC + Clang/LLD, static ABI/security audit and
# QEMU execution of the test-owned, in-memory, sectionless ELF fixture.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C

NXLOADER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOLCHAIN_ROOT=${NXLOADER_ARMHF_TOOLCHAIN_ROOT:-/opt/prebuilt}
GCC=${NXLOADER_ARMHF_GCC:-$TOOLCHAIN_ROOT/bin/arm-linux-gnueabihf-gcc}
SYSROOT=${NXLOADER_ARMHF_SYSROOT:-}
CLANG=${NXLOADER_ARMHF_CLANG:-$(command -v clang 2>/dev/null || true)}
LLD=${NXLOADER_ARMHF_LLD:-$(command -v ld.lld 2>/dev/null || true)}
QEMU=${NXLOADER_QEMU_ARM:-$(command -v qemu-arm 2>/dev/null || true)}
READELF=${NXLOADER_READELF:-$(command -v readelf 2>/dev/null || true)}
INTERPRETER=/lib/ld-linux-armhf.so.3
WORK=$(mktemp -d /tmp/nxloader-m11-armv7-lifecycle.XXXXXX)
GCC_EXECUTABLE=$WORK/m11-armv7-gcc
CLANG_EXECUTABLE=$WORK/m11-armv7-clang-lld
MAX_GLIBC=none

cleanup() {
  local status=$?
  trap - EXIT
  case $WORK in
    /tmp/nxloader-m11-armv7-lifecycle.??????)
      chmod -R u+w -- "$WORK" 2>/dev/null || true
      find "$WORK" -depth -delete 2>/dev/null || true
      ;;
    *)
      printf 'm11-armv7-lifecycle-cross: refused cleanup outside owned mktemp: %s\n' \
        "$WORK" >&2
      status=1
      ;;
  esac
  if [[ -e $WORK ]]; then
    printf 'm11-armv7-lifecycle-cross: cleanup_failed=%s\n' "$WORK" >&2
    [[ $status -ne 0 ]] || status=1
  else
    printf 'm11-armv7-lifecycle-cross: work_tree_cleaned=1\n'
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() {
  printf 'm11-armv7-lifecycle-cross: FAIL %s\n' "$*" >&2
  exit 1
}

for utility in awk chmod dirname env find grep head mktemp od sed sha256sum \
               sort tr; do
  command -v "$utility" >/dev/null 2>&1 || fail "missing command: $utility"
done
for utility in "$GCC" "$CLANG" "$LLD" "$QEMU" "$READELF"; do
  [[ -n $utility && -x $utility ]] ||
    fail "missing executable: ${utility:-empty}"
done

if [[ -z $SYSROOT ]]; then
  SYSROOT=$($GCC -print-sysroot)
fi
SYSROOT=$(cd -- "$SYSROOT" && pwd -P)
[[ -d $SYSROOT ]] || fail "missing ARMHF sysroot: $SYSROOT"
[[ -r $SYSROOT$INTERPRETER ]] || fail "sysroot lacks $INTERPRETER"
[[ -r $SYSROOT/lib/libc.so.6 ]] || fail "sysroot lacks libc.so.6"

sources=(
  "$NXLOADER_ROOT/tests/test_m11_lifecycle_cross.c"
  "$NXLOADER_ROOT/src/nxloader.c"
  "$NXLOADER_ROOT/src/nxloader_elf32.c"
  "$NXLOADER_ROOT/src/nxloader_elf64.c"
  "$NXLOADER_ROOT/src/nxloader_registry.c"
  "$NXLOADER_ROOT/src/nxloader_hooks.c"
  "$NXLOADER_ROOT/src/nxloader_protect.c"
)
common_flags=(
  -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror
  -fno-omit-frame-pointer -march=armv7-a -marm -mfpu=neon
  -mfloat-abi=hard -I"$NXLOADER_ROOT/include" -I"$NXLOADER_ROOT/src"
  -Wl,-z,noexecstack
)

version_is_above() {
  awk -v found="$1" -v limit="$2" 'BEGIN {
    split(found, f, "."); split(limit, l, ".");
    fn = (f[1] + 0) * 1000000 + (f[2] + 0) * 1000 + (f[3] + 0);
    ln = (l[1] + 0) * 1000000 + (l[2] + 0) * 1000 + (l[3] + 0);
    exit !(fn > ln)
  }'
}

update_max_glibc() {
  local candidate=$1
  if [[ $MAX_GLIBC == none ]] ||
     version_is_above "$candidate" "$MAX_GLIBC"; then
    MAX_GLIBC=$candidate
  fi
}

resolve_needed() {
  local soname=$1 directory
  for directory in \
    "$SYSROOT/lib" \
    "$SYSROOT/usr/lib" \
    "$TOOLCHAIN_ROOT/arm-linux-gnueabihf/lib"; do
    [[ -e $directory/$soname ]] && return 0
  done
  return 1
}

audit_executable() {
  local elf=$1 header attributes programs dynamic version_info versions
  local type interpreter_count interpreter stack_count stack_line version needed
  header=$($READELF -hW -- "$elf") || fail "cannot read ELF header: $elf"
  grep -Eq '^[[:space:]]*Class:[[:space:]]+ELF32$' <<<"$header" ||
    fail "not ELF32: $elf"
  grep -Eq "^[[:space:]]*Data:[[:space:]]+2's complement, little endian$" \
    <<<"$header" || fail "not little-endian: $elf"
  grep -Eq '^[[:space:]]*Machine:[[:space:]]+ARM$' <<<"$header" ||
    fail "not EM_ARM: $elf"
  grep -Eq '^[[:space:]]*Flags:.*Version5 EABI' <<<"$header" ||
    fail "not ARM EABI5: $elf"
  grep -Eq '^[[:space:]]*Flags:.*hard-float ABI' <<<"$header" ||
    fail "not ARM hard-float: $elf"
  if grep -Eq '^[[:space:]]*Flags:.*soft-float ABI' <<<"$header"; then
    fail "conflicting ARM soft-float flag: $elf"
  fi
  type=$(sed -n \
    's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*$/\1/p' \
    <<<"$header")
  [[ $type == EXEC || $type == DYN ]] ||
    fail "unexpected executable type ${type:-unknown}: $elf"

  attributes=$($READELF -AW -- "$elf") ||
    fail "cannot read ARM attributes: $elf"
  grep -Eq 'Tag_ABI_VFP_args:[[:space:]]+VFP registers' <<<"$attributes" ||
    fail "missing hard-float Tag_ABI_VFP_args: $elf"

  programs=$($READELF -lW -- "$elf") ||
    fail "cannot read program headers: $elf"
  interpreter_count=$(grep -c 'Requesting program interpreter:' \
    <<<"$programs" || true)
  (( interpreter_count == 1 )) || fail "expected one PT_INTERP: $elf"
  interpreter=$(sed -n \
    's/^.*Requesting program interpreter: \([^]]*\)].*$/\1/p' \
    <<<"$programs")
  [[ $interpreter == "$INTERPRETER" ]] ||
    fail "wrong PT_INTERP in $elf: ${interpreter:-none}"
  stack_count=$(grep -c '^[[:space:]]*GNU_STACK' <<<"$programs" || true)
  (( stack_count == 1 )) || fail "expected one GNU_STACK: $elf"
  stack_line=$(grep '^[[:space:]]*GNU_STACK' <<<"$programs")
  if grep -Eq '[[:space:]]RWE[[:space:]]' <<<"$stack_line"; then
    fail "executable GNU_STACK: $elf"
  fi
  if grep -Eq '^[[:space:]]*LOAD .*RWE[[:space:]]' <<<"$programs"; then
    fail "writable+executable PT_LOAD: $elf"
  fi

  dynamic=$($READELF -dW -- "$elf" 2>/dev/null || true)
  if grep -Eq '\((RPATH|RUNPATH)\)' <<<"$dynamic"; then
    fail "RPATH/RUNPATH is forbidden: $elf"
  fi
  if grep -Eq '\(TEXTREL\)|FLAGS.*TEXTREL' <<<"$dynamic"; then
    fail "text relocations are forbidden: $elf"
  fi
  version_info=$($READELF --version-info -W -- "$elf" 2>/dev/null || true)
  versions=$(grep -oE 'GLIBC_[0-9]+([.][0-9]+)+' <<<"$version_info" |
    sed 's/^GLIBC_//' | sort -Vu || true)
  while IFS= read -r version; do
    [[ -n $version ]] || continue
    if version_is_above "$version" 2.30; then
      fail "$elf requires GLIBC_$version (maximum GLIBC_2.30)"
    fi
    update_max_glibc "$version"
  done <<<"$versions"
  if grep -q 'GLIBC_PRIVATE' <<<"$version_info"; then
    fail "$elf requires GLIBC_PRIVATE"
  fi
  while IFS= read -r needed; do
    [[ -n $needed ]] || continue
    resolve_needed "$needed" ||
      fail "$elf has unresolved DT_NEEDED in ARMHF sysroot: $needed"
  done < <(sed -n 's/^.*(NEEDED).*\[\([^]]*\)\].*$/\1/p' <<<"$dynamic")
}

audit_lld_linker() {
  local comments
  comments=$($READELF -p .comment -- "$1" 2>/dev/null || true)
  grep -Fq 'Linker: LLD ' <<<"$comments" ||
    fail "Clang output does not prove LLD linkage: $1"
}

printf 'm11-armv7-lifecycle-cross: gcc=%s\n' "$($GCC --version | head -n 1)"
printf 'm11-armv7-lifecycle-cross: clang=%s\n' "$($CLANG --version | head -n 1)"
printf 'm11-armv7-lifecycle-cross: lld=%s\n' "$($LLD --version | head -n 1)"
printf 'm11-armv7-lifecycle-cross: qemu=%s\n' "$($QEMU --version | head -n 1)"
printf 'm11-armv7-lifecycle-cross: manifest_begin\n'
sha256sum -- "${sources[@]}" "$NXLOADER_ROOT/include/nxloader.h" \
  "$NXLOADER_ROOT/src/nxloader_internal.h" "${BASH_SOURCE[0]}" \
  "$GCC" "$CLANG" "$LLD" "$QEMU" "$SYSROOT$INTERPRETER" \
  "$SYSROOT/lib/libc.so.6"
printf 'm11-armv7-lifecycle-cross: manifest_end\n'

"$GCC" --sysroot="$SYSROOT" "${common_flags[@]}" "${sources[@]}" \
  -o "$GCC_EXECUTABLE"
"$CLANG" --target=arm-linux-gnueabihf --sysroot="$SYSROOT" \
  --gcc-toolchain="$TOOLCHAIN_ROOT" -fuse-ld=lld \
  "${common_flags[@]}" "${sources[@]}" -o "$CLANG_EXECUTABLE"

audit_executable "$GCC_EXECUTABLE"
audit_executable "$CLANG_EXECUTABLE"
audit_lld_linker "$CLANG_EXECUTABLE"
[[ $MAX_GLIBC != none ]] || fail "no GLIBC version requirement measured"

runtime_expected='m11-armv7-lifecycle-cross: PASS sectionless=1 lifecycle=1 dt_init_order=1 init_array1_order=2 init_array2_order=3 init_array_entries=4 init_array_sentinels_ignored=2 initializers_exactly_once=1 jni_order=4 jni_version=0x00010004 jni_literal_lookup=1 jni_exactly_once=1 ready=1 stack_align=8 wx_mapping=0 relro=1 test_owned_guest=1 external_guest=0 device_access=0 network_access=0 hardware_ran=0'
if ! gcc_output=$(env -i LC_ALL=C "$QEMU" -L "$SYSROOT" \
    "$GCC_EXECUTABLE" 2>&1); then
  printf '%s\n' "$gcc_output" >&2
  fail "GCC QEMU execution failed"
fi
if ! clang_output=$(env -i LC_ALL=C "$QEMU" -L "$SYSROOT" \
    "$CLANG_EXECUTABLE" 2>&1); then
  printf '%s\n' "$clang_output" >&2
  fail "Clang/LLD QEMU execution failed"
fi
printf '%s\n' "$gcc_output"
printf '%s\n' "$clang_output"
[[ $gcc_output == "$runtime_expected" ]] || fail "GCC runtime mismatch"
[[ $clang_output == "$runtime_expected" ]] ||
  fail "Clang/LLD runtime mismatch"

printf 'm11-armv7-lifecycle-cross: PASS gcc=1 clang=1 lld=1 qemu=1 '
printf 'lifecycle_runs=2 sectionless=2 dt_init_order=2 init_array_order=2 '
printf 'init_array_sentinels_ignored=4 initializers_exactly_once=2 '
printf 'jni_order=2 jni_1_4=2 jni_exact_whitelist=2 jni_literal_lookup=2 '
printf 'jni_exactly_once=2 ready=2 hard_float=2 stack_align_8=2 '
printf 'wx_mapping=0 relro=2 glibc_max=%s pt_interp=%s ' \
  "$MAX_GLIBC" "$INTERPRETER"
printf 'test_owned_guest=2 external_guest=0 device_access=0 '
printf 'network_access=0 hardware_ran=0\n'
