#!/usr/bin/env bash
# ARMHF cross gate: GCC + Clang/LLD, static ABI audit and QEMU execution.
# All build outputs live in an owned mktemp directory.  The executable under
# test never loads an ELF guest and never calls a guest initializer.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C

NXLOADER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
ARMHF_TOOLCHAIN_ROOT=${NXLOADER_ARMHF_TOOLCHAIN_ROOT:-/opt/prebuilt}
ARMHF_GCC=${NXLOADER_ARMHF_GCC:-$ARMHF_TOOLCHAIN_ROOT/bin/arm-linux-gnueabihf-gcc}
ARMHF_SYSROOT=${NXLOADER_ARMHF_SYSROOT:-}
ARMHF_CLANG=${NXLOADER_ARMHF_CLANG:-$(command -v clang 2>/dev/null || true)}
ARMHF_LLD=${NXLOADER_ARMHF_LLD:-$(command -v ld.lld 2>/dev/null || true)}
ARMHF_QEMU=${NXLOADER_QEMU_ARM:-$(command -v qemu-arm 2>/dev/null || true)}
READELF=${NXLOADER_READELF:-$(command -v readelf 2>/dev/null || true)}
ARMHF_OBJDUMP=${NXLOADER_ARMHF_OBJDUMP:-$ARMHF_TOOLCHAIN_ROOT/bin/arm-linux-gnueabihf-objdump}
ARMHF_WORK=$(mktemp -d /tmp/nxloader-armv7-cross.XXXXXX)
ARMHF_GCC_BUILD=$ARMHF_WORK/gcc
ARMHF_CLANG_BUILD=$ARMHF_WORK/clang
ARMHF_INTERPRETER=/lib/ld-linux-armhf.so.3
ELF_COUNT=0
ELF_LOADABLE_COUNT=0
ELF_RELOCATABLE_COUNT=0
MAX_GLIBC=none

cleanup() {
  local status=$?
  trap - EXIT
  case $ARMHF_WORK in
    /tmp/nxloader-armv7-cross.??????)
      chmod -R u+w -- "$ARMHF_WORK" 2>/dev/null || true
      rm -rf -- "$ARMHF_WORK"
      ;;
    *)
      printf 'armv7-cross: refused cleanup outside owned mktemp: %s\n' \
        "$ARMHF_WORK" >&2
      status=1
      ;;
  esac
  if [[ -e $ARMHF_WORK ]]; then
    printf 'armv7-cross: cleanup_failed=%s\n' "$ARMHF_WORK" >&2
    [[ $status -ne 0 ]] || status=1
  else
    printf 'armv7-cross: work_tree_cleaned=1\n'
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() {
  printf 'armv7-cross: FAIL %s\n' "$*" >&2
  exit 1
}

for utility in awk chmod cmake find grep head mktemp ninja od rm sed sha256sum \
               sort tr; do
  command -v "$utility" >/dev/null 2>&1 || fail "missing command: $utility"
done
for utility in "$ARMHF_GCC" "$ARMHF_CLANG" "$ARMHF_LLD" "$ARMHF_QEMU" \
               "$READELF" "$ARMHF_OBJDUMP"; do
  [[ -n $utility && -x $utility ]] || fail "missing executable: ${utility:-empty}"
done

if [[ -z $ARMHF_SYSROOT ]]; then
  ARMHF_SYSROOT=$($ARMHF_GCC -print-sysroot)
fi
ARMHF_SYSROOT=$(cd -- "$ARMHF_SYSROOT" && pwd -P)
[[ -d $ARMHF_SYSROOT ]] || fail "missing ARMHF sysroot: $ARMHF_SYSROOT"
[[ -r $ARMHF_SYSROOT$ARMHF_INTERPRETER ]] ||
  fail "sysroot lacks $ARMHF_INTERPRETER"

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
  if [[ $MAX_GLIBC == none ]] || version_is_above "$candidate" "$MAX_GLIBC"; then
    MAX_GLIBC=$candidate
  fi
}

is_elf() {
  local magic
  magic=$(od -An -N4 -t x1 -- "$1" 2>/dev/null | tr -d ' \n')
  [[ $magic == 7f454c46 ]]
}

resolve_needed() {
  local soname=$1 directory
  for directory in \
    "$ARMHF_SYSROOT/lib" \
    "$ARMHF_SYSROOT/usr/lib" \
    "$ARMHF_TOOLCHAIN_ROOT/arm-linux-gnueabihf/lib"; do
    [[ -e $directory/$soname ]] && return 0
  done
  return 1
}

audit_elf() {
  local elf=$1 header attributes programs dynamic type interpreter_count
  local interpreter version_info versions version needed
  header=$($READELF -hW -- "$elf") || fail "readelf header failed: $elf"
  grep -Eq '^[[:space:]]*Class:[[:space:]]+ELF32$' <<<"$header" ||
    fail "not ELF32: $elf"
  grep -Eq "^[[:space:]]*Data:[[:space:]]+2's complement, little endian$" \
    <<<"$header" || fail "not little-endian: $elf"
  grep -Eq '^[[:space:]]*Machine:[[:space:]]+ARM$' <<<"$header" ||
    fail "not EM_ARM: $elf"
  grep -Eq '^[[:space:]]*Flags:.*Version5 EABI' <<<"$header" ||
    fail "not ARM EABI5: $elf"
  if grep -Eq '^[[:space:]]*Flags:.*soft-float ABI' <<<"$header"; then
    fail "ELF declares soft-float or conflicting float ABI: $elf"
  fi

  type=$(sed -n 's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*$/\1/p' \
    <<<"$header")
  case $type in
    EXEC|DYN)
      # The ARM ELF ABI carries the hard-float bit in loadable outputs.  GNU
      # ld deliberately leaves both float bits clear in ET_REL objects and
      # records their PCS in .ARM.attributes instead, so applying this test
      # to .o files would reject normal hard-float compiler output.
      grep -Eq '^[[:space:]]*Flags:.*hard-float ABI' <<<"$header" ||
        fail "loadable ELF is not ARM hard-float: $elf"
      ELF_LOADABLE_COUNT=$((ELF_LOADABLE_COUNT + 1))
      ;;
    REL)
      ELF_RELOCATABLE_COUNT=$((ELF_RELOCATABLE_COUNT + 1))
      ;;
    *)
      fail "unexpected ARM ELF type ${type:-unknown}: $elf"
      ;;
  esac

  attributes=$($READELF -AW -- "$elf") ||
    fail "ARM attributes unreadable: $elf"
  grep -Eq 'Tag_ABI_VFP_args:[[:space:]]+VFP registers' <<<"$attributes" ||
    fail "missing hard-float Tag_ABI_VFP_args: $elf"

  programs=$($READELF -lW -- "$elf" 2>/dev/null || true)
  interpreter_count=$(grep -c 'Requesting program interpreter:' \
    <<<"$programs" || true)
  if (( interpreter_count > 0 )); then
    (( interpreter_count == 1 )) || fail "multiple PT_INTERP entries: $elf"
    interpreter=$(sed -n \
      's/^.*Requesting program interpreter: \([^]]*\)].*$/\1/p' \
      <<<"$programs")
    [[ $interpreter == "$ARMHF_INTERPRETER" ]] ||
      fail "wrong PT_INTERP in $elf: ${interpreter:-none}"
  fi

  if [[ $type == EXEC ]]; then
    (( interpreter_count == 1 )) || fail "ARMHF executable has no PT_INTERP: $elf"
  fi

  dynamic=$($READELF -dW -- "$elf" 2>/dev/null || true)
  if grep -Eq '\((RPATH|RUNPATH)\)' <<<"$dynamic"; then
    fail "RPATH/RUNPATH is forbidden: $elf"
  fi
  if grep -Eq '\(TEXTREL\)|FLAGS.*TEXTREL' <<<"$dynamic"; then
    fail "text relocation marker is forbidden: $elf"
  fi

  version_info=$($READELF --version-info -W -- "$elf" 2>/dev/null || true)
  versions=$(grep -oE 'GLIBC_[0-9]+([.][0-9]+)+' <<<"$version_info" |
    sed 's/^GLIBC_//' | sort -Vu || true)
  while IFS= read -r version; do
    [[ -n $version ]] || continue
    version_is_above "$version" 2.30 &&
      fail "$elf requires GLIBC_$version (maximum GLIBC_2.30)"
    update_max_glibc "$version"
  done <<<"$versions"
  if grep -q 'GLIBC_PRIVATE' <<<"$version_info"; then
    fail "$elf requires GLIBC_PRIVATE"
  fi

  while IFS= read -r needed; do
    [[ -n $needed ]] || continue
    resolve_needed "$needed" ||
      fail "$elf has unresolved DT_NEEDED in cross sysroot: $needed"
  done < <(sed -n 's/^.*(NEEDED).*\[\([^]]*\)\].*$/\1/p' <<<"$dynamic")

  ELF_COUNT=$((ELF_COUNT + 1))
}

audit_tree() {
  local tree=$1 elf
  while IFS= read -r -d '' elf; do
    is_elf "$elf" || continue
    audit_elf "$elf"
  done < <(find "$tree" -type f -print0)
}

audit_final_executable() {
  local elf=$1 programs count
  [[ -f $elf ]] || fail "missing cross-safe executable: $elf"
  programs=$($READELF -lW -- "$elf") || fail "cannot read final PT_INTERP"
  count=$(grep -c "Requesting program interpreter: $ARMHF_INTERPRETER" \
    <<<"$programs" || true)
  (( count == 1 )) || fail "final executable lacks canonical ARMHF PT_INTERP: $elf"
}

audit_vfp_probe_disassembly() {
  local elf=$1 disassembly probe
  disassembly=$($ARMHF_OBJDUMP -d -- "$elf") ||
    fail "cannot disassemble VFP probe: $elf"
  probe=$(sed -n '/<nx_vfp_d8_preserved>:/,/^$/p' <<<"$disassembly")
  [[ -n $probe ]] || fail "VFP d8 probe symbol missing: $elf"
  grep -Eq 'vpush[[:space:]]+\{d8\}' <<<"$probe" ||
    fail "VFP d8 probe does not save d8: $elf"
  grep -Eq 'vmov[[:space:]]+d8,[[:space:]]*r2,[[:space:]]*r3' <<<"$probe" ||
    fail "VFP d8 probe does not seed d8: $elf"
  grep -Eq 'blx[[:space:]]+r4' <<<"$probe" ||
    fail "VFP d8 probe does not call the registry thunk: $elf"
  grep -Eq 'vmov[[:space:]]+r2,[[:space:]]*r3,[[:space:]]*d8' <<<"$probe" ||
    fail "VFP d8 probe does not read d8 after the thunk: $elf"
  grep -Eq 'vpop[[:space:]]+\{d8\}' <<<"$probe" ||
    fail "VFP d8 probe does not restore d8: $elf"
}

audit_lld_linker() {
  local elf=$1 comments
  comments=$($READELF -p .comment -- "$elf" 2>/dev/null || true)
  grep -Fq 'Linker: LLD ' <<<"$comments" ||
    fail "Clang output does not prove an LLD link: $elf"
}

build_common=(
  -S "$NXLOADER_ROOT"
  -G Ninja
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR=armv7
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_SYSROOT="$ARMHF_SYSROOT"
  '-DCMAKE_C_FLAGS_INIT=-march=armv7-a -marm -mfpu=neon -mfloat-abi=hard -fno-omit-frame-pointer'
  -DNXLOADER_BUILD_TESTS=ON
  -DNXLOADER_BUILD_TOOLS=OFF
  -DNXLOADER_BUILD_SOFTFP=ON
  -DNXLOADER_BUILD_ARMV7_CROSS_TEST=ON
  -DNXLOADER_BUILD_FUZZER=OFF
  -DNXLOADER_ENABLE_SANITIZERS=OFF
  -DNXLOADER_WARNINGS_AS_ERRORS=ON
)

printf 'armv7-cross: gcc=%s\n' "$($ARMHF_GCC --version | head -n 1)"
printf 'armv7-cross: clang=%s\n' "$($ARMHF_CLANG --version | head -n 1)"
printf 'armv7-cross: lld=%s\n' "$($ARMHF_LLD --version | head -n 1)"
printf 'armv7-cross: qemu=%s\n' "$($ARMHF_QEMU --version | head -n 1)"
printf 'armv7-cross: sysroot=%s\n' "$ARMHF_SYSROOT"
sha256sum -- "$ARMHF_GCC" "$ARMHF_CLANG" "$ARMHF_LLD" "$ARMHF_QEMU" \
  "$ARMHF_OBJDUMP" "$ARMHF_SYSROOT$ARMHF_INTERPRETER" \
  "$ARMHF_SYSROOT/lib/libc.so.6"

cmake "${build_common[@]}" -B "$ARMHF_GCC_BUILD" \
  -DCMAKE_C_COMPILER="$ARMHF_GCC"
cmake --build "$ARMHF_GCC_BUILD" --target \
  nxloader_armv7_cross_test nxloader_softfp_tests \
  --parallel

cmake "${build_common[@]}" -B "$ARMHF_CLANG_BUILD" \
  -DCMAKE_C_COMPILER="$ARMHF_CLANG" \
  -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf \
  -DCMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN="$ARMHF_TOOLCHAIN_ROOT" \
  -DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=lld
cmake --build "$ARMHF_CLANG_BUILD" --target \
  nxloader_armv7_cross_test nxloader_softfp_tests \
  --parallel

audit_tree "$ARMHF_GCC_BUILD"
audit_tree "$ARMHF_CLANG_BUILD"
audit_final_executable "$ARMHF_GCC_BUILD/nxloader_armv7_cross_test"
audit_final_executable "$ARMHF_CLANG_BUILD/nxloader_armv7_cross_test"
audit_vfp_probe_disassembly "$ARMHF_GCC_BUILD/nxloader_armv7_cross_test"
audit_vfp_probe_disassembly "$ARMHF_CLANG_BUILD/nxloader_armv7_cross_test"
audit_lld_linker "$ARMHF_CLANG_BUILD/nxloader_armv7_cross_test"

gcc_output=$($ARMHF_QEMU -L "$ARMHF_SYSROOT" \
  "$ARMHF_GCC_BUILD/nxloader_armv7_cross_test")
clang_output=$($ARMHF_QEMU -L "$ARMHF_SYSROOT" \
  "$ARMHF_CLANG_BUILD/nxloader_armv7_cross_test")
gcc_softfp_output=$($ARMHF_QEMU -L "$ARMHF_SYSROOT" \
  "$ARMHF_GCC_BUILD/nxloader_softfp_tests")
clang_softfp_output=$($ARMHF_QEMU -L "$ARMHF_SYSROOT" \
  "$ARMHF_CLANG_BUILD/nxloader_softfp_tests")
printf '%s\n' "$gcc_output"
printf '%s\n' "$clang_output"
printf '%s\n' "$gcc_softfp_output"
printf '%s\n' "$clang_softfp_output"
grep -Fq 'armv7-cross: PASS' <<<"$gcc_output" || fail "GCC QEMU test failed"
grep -Fq 'armv7-cross: PASS' <<<"$clang_output" || fail "Clang QEMU test failed"
grep -Fq 'nxloader: ARMv7 softfp provider test passed' \
  <<<"$gcc_softfp_output" || fail "GCC softfp provider test failed"
grep -Fq 'nxloader: ARMv7 softfp provider test passed' \
  <<<"$clang_softfp_output" || fail "Clang softfp provider test failed"

printf 'armv7-cross: PASS gcc=1 clang=1 lld=1 qemu=1 '
printf 'softfp_provider_tests=2 vfp_disassembly=2 '
printf 'elf_count=%d ' "$ELF_COUNT"
printf 'loadable_elf_count=%d relocatable_elf_count=%d glibc_max=%s ' \
  "$ELF_LOADABLE_COUNT" "$ELF_RELOCATABLE_COUNT" "$MAX_GLIBC"
printf 'hardware_ran=0 device_access=0 guest_elf_loaded=0 '
printf 'guest_initializers_executed=0\n'
