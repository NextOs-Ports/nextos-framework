#!/usr/bin/env bash
# M11 AArch64 lifecycle gate: pinned low-glibc GCC, Clang/LLD, static
# ABI/security audit and QEMU execution of a test-owned sectionless ELF.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C

NXLOADER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
BUSTER_IMAGE=${NXLOADER_AARCH64_BUSTER_IMAGE:-playfetch-builder:buster}
BUSTER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
DOCKER=${NXLOADER_DOCKER:-$(command -v docker 2>/dev/null || true)}
CLANG=${NXLOADER_AARCH64_CLANG:-$(command -v clang 2>/dev/null || true)}
LLD=${NXLOADER_AARCH64_LLD:-$(command -v ld.lld 2>/dev/null || true)}
QEMU=${NXLOADER_QEMU_AARCH64:-$(command -v qemu-aarch64 2>/dev/null || true)}
READELF=${NXLOADER_READELF:-$(command -v readelf 2>/dev/null || true)}
WORK=$(mktemp -d /tmp/nxloader-m11-aarch64-lifecycle.XXXXXX)
EXPORT_ROOT=$WORK/buster-root
SYSROOT=$EXPORT_ROOT/usr/aarch64-linux-gnu
GCC_INSTALL=$EXPORT_ROOT/usr/lib/gcc-cross/aarch64-linux-gnu/8
GCC_EXECUTABLE=$WORK/m11-aarch64-gcc
CLANG_EXECUTABLE=$WORK/m11-aarch64-clang-lld
INTERPRETER=/lib/ld-linux-aarch64.so.1
MAX_GLIBC=none

cleanup() {
  local status=$?
  trap - EXIT
  case $WORK in
    /tmp/nxloader-m11-aarch64-lifecycle.??????)
      chmod -R u+w -- "$WORK" 2>/dev/null || true
      find "$WORK" -depth -delete 2>/dev/null || true
      ;;
    *)
      printf 'm11-aarch64-lifecycle-cross: refused cleanup outside owned mktemp: %s\n' \
        "$WORK" >&2
      status=1
      ;;
  esac
  if [[ -e $WORK ]]; then
    printf 'm11-aarch64-lifecycle-cross: cleanup_failed=%s\n' "$WORK" >&2
    [[ $status -ne 0 ]] || status=1
  else
    printf 'm11-aarch64-lifecycle-cross: work_tree_cleaned=1\n'
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() {
  printf 'm11-aarch64-lifecycle-cross: FAIL %s\n' "$*" >&2
  exit 1
}

for utility in awk chmod dirname docker env find grep head id mkdir mktemp \
               sed sha256sum sort; do
  command -v "$utility" >/dev/null 2>&1 || fail "missing command: $utility"
done
for utility in "$DOCKER" "$CLANG" "$LLD" "$QEMU" "$READELF"; do
  [[ -n $utility && -x $utility ]] ||
    fail "missing executable: ${utility:-empty}"
done

resolved_image_id=$($DOCKER image inspect --format '{{.Id}}' "$BUSTER_IMAGE") ||
  fail "cannot inspect pinned Buster image"
[[ $resolved_image_id == "$BUSTER_IMAGE_ID" ]] ||
  fail "Buster image drift: $resolved_image_id != $BUSTER_IMAGE_ID"

docker_base=(
  "$DOCKER" run --rm
  --network none
  --read-only
  --cap-drop ALL
  --security-opt no-new-privileges:true
  --pids-limit 256
  --user "$(id -u):$(id -g)"
  --tmpfs /tmp:rw,nosuid,nodev,noexec,size=256m
)

sources=(
  "$NXLOADER_ROOT/tests/test_m11_lifecycle_cross.c"
  "$NXLOADER_ROOT/src/nxloader.c"
  "$NXLOADER_ROOT/src/nxloader_elf32.c"
  "$NXLOADER_ROOT/src/nxloader_elf64.c"
  "$NXLOADER_ROOT/src/nxloader_registry.c"
  "$NXLOADER_ROOT/src/nxloader_hooks.c"
  "$NXLOADER_ROOT/src/nxloader_protect.c"
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
  for directory in "$SYSROOT/lib" "$SYSROOT/usr/lib" "$GCC_INSTALL"; do
    [[ -e $directory/$soname ]] && return 0
  done
  return 1
}

audit_executable() {
  local elf=$1 header type programs interpreter_count interpreter
  local stack_count stack_line dynamic version_info versions version needed
  header=$($READELF -hW -- "$elf") || fail "cannot read ELF header: $elf"
  grep -Eq '^[[:space:]]*Class:[[:space:]]+ELF64$' <<<"$header" ||
    fail "not ELF64: $elf"
  grep -Eq "^[[:space:]]*Data:[[:space:]]+2's complement, little endian$" \
    <<<"$header" || fail "not little-endian: $elf"
  grep -Eq '^[[:space:]]*Machine:[[:space:]]+AArch64$' <<<"$header" ||
    fail "not EM_AARCH64: $elf"
  grep -Eq '^[[:space:]]*Flags:[[:space:]]+0x0$' <<<"$header" ||
    fail "unexpected AArch64 e_flags: $elf"
  type=$(sed -n \
    's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*$/\1/p' \
    <<<"$header")
  [[ $type == EXEC || $type == DYN ]] ||
    fail "unexpected executable type ${type:-unknown}: $elf"

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
      fail "$elf has unresolved DT_NEEDED in Buster sysroot: $needed"
  done < <(sed -n 's/^.*(NEEDED).*\[\([^]]*\)\].*$/\1/p' <<<"$dynamic")
}

audit_lld_linker() {
  local comments
  comments=$($READELF -p .comment -- "$1" 2>/dev/null || true)
  grep -Fq 'Linker: LLD ' <<<"$comments" ||
    fail "Clang output does not prove LLD linkage: $1"
}

mkdir -p -- "$EXPORT_ROOT/usr/lib/gcc-cross/aarch64-linux-gnu"
"${docker_base[@]}" \
  --volume "$EXPORT_ROOT:/export:rw" \
  "$BUSTER_IMAGE" bash -ceu '
    umask 022
    cp -a --no-preserve=ownership /usr/aarch64-linux-gnu /export/usr/
    cp -a --no-preserve=ownership \
      /usr/lib/gcc-cross/aarch64-linux-gnu/8 \
      /export/usr/lib/gcc-cross/aarch64-linux-gnu/
  '
[[ -d $SYSROOT && -d $GCC_INSTALL ]] ||
  fail "private Buster toolchain export is incomplete"
[[ -r $SYSROOT$INTERPRETER ]] || fail "private sysroot lacks $INTERPRETER"
[[ -r $SYSROOT/lib/libc.so.6 ]] || fail "private sysroot lacks libc.so.6"

printf 'm11-aarch64-lifecycle-cross: image=%s image_id=%s network=none source_ro=1\n' \
  "$BUSTER_IMAGE" "$resolved_image_id"
printf 'm11-aarch64-lifecycle-cross: clang=%s\n' "$($CLANG --version | head -n 1)"
printf 'm11-aarch64-lifecycle-cross: lld=%s\n' "$($LLD --version | head -n 1)"
printf 'm11-aarch64-lifecycle-cross: qemu=%s\n' "$($QEMU --version | head -n 1)"
printf 'm11-aarch64-lifecycle-cross: manifest_begin\n'
printf 'image_id=%s\n' "$resolved_image_id"
sha256sum -- "${sources[@]}" "$NXLOADER_ROOT/include/nxloader.h" \
  "$NXLOADER_ROOT/src/nxloader_internal.h" "${BASH_SOURCE[0]}" \
  "$CLANG" "$LLD" "$QEMU" "$SYSROOT$INTERPRETER" \
  "$SYSROOT/lib/libc.so.6"
"${docker_base[@]}" "$BUSTER_IMAGE" bash -ceu '
  aarch64-linux-gnu-gcc --version | head -n 1
  sha256sum /usr/bin/aarch64-linux-gnu-gcc \
    /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 \
    /usr/aarch64-linux-gnu/lib/libc.so.6
'
printf 'm11-aarch64-lifecycle-cross: manifest_end\n'

"${docker_base[@]}" \
  --volume "$NXLOADER_ROOT:/src:ro" \
  --volume "$WORK:/work:rw" \
  --workdir /work \
  "$BUSTER_IMAGE" bash -ceu '
    test ! -w /src/tests/test_m11_lifecycle_cross.c
    aarch64-linux-gnu-gcc \
      -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
      -fno-omit-frame-pointer -Wl,-z,noexecstack \
      -I/src/include -I/src/src \
      /src/tests/test_m11_lifecycle_cross.c \
      /src/src/nxloader.c \
      /src/src/nxloader_elf32.c \
      /src/src/nxloader_elf64.c \
      /src/src/nxloader_registry.c \
      /src/src/nxloader_hooks.c \
      /src/src/nxloader_protect.c \
      -o /work/m11-aarch64-gcc
  '

"$CLANG" --target=aarch64-linux-gnu --sysroot="$EXPORT_ROOT" \
  --gcc-install-dir="$GCC_INSTALL" -isystem "$SYSROOT/include" \
  -fuse-ld=lld -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -Wl,-z,noexecstack \
  -I"$NXLOADER_ROOT/include" -I"$NXLOADER_ROOT/src" \
  "${sources[@]}" -o "$CLANG_EXECUTABLE"

audit_executable "$GCC_EXECUTABLE"
audit_executable "$CLANG_EXECUTABLE"
audit_lld_linker "$CLANG_EXECUTABLE"
[[ $MAX_GLIBC != none ]] || fail "no GLIBC version requirement measured"

runtime_expected='m11-aarch64-lifecycle-cross: PASS sectionless=1 lifecycle=1 dt_init_order=1 init_array1_order=2 init_array2_order=3 init_array_entries=4 init_array_sentinels_ignored=2 initializers_exactly_once=1 jni_order=4 jni_version=0x00010006 jni_literal_lookup=1 jni_exactly_once=1 ready=1 stack_align=16 wx_mapping=0 relro=1 test_owned_guest=1 external_guest=0 device_access=0 network_access=0 hardware_ran=0'
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

printf 'm11-aarch64-lifecycle-cross: PASS gcc=1 clang=1 lld=1 qemu=1 '
printf 'lifecycle_runs=2 sectionless=2 dt_init_order=2 init_array_order=2 '
printf 'init_array_sentinels_ignored=4 initializers_exactly_once=2 '
printf 'jni_order=2 jni_1_6=2 jni_exact_whitelist=2 jni_literal_lookup=2 '
printf 'jni_exactly_once=2 ready=2 lp64=2 stack_align_16=2 '
printf 'wx_mapping=0 relro=2 glibc_max=%s pt_interp=%s ' \
  "$MAX_GLIBC" "$INTERPRETER"
printf 'image_id=%s source_ro=1 ' "$resolved_image_id"
printf 'test_owned_guest=2 external_guest=0 device_access=0 '
printf 'network_access=0 hardware_ran=0\n'
