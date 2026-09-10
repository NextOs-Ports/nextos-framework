#!/usr/bin/env bash
# Reproduz o pequeno adapter NextOS embutido em src/ab_slingshot_adapter.inc.
# Ele contém apenas código GPL do port; o envelope corresponde ao formato que
# o loader legítimo de Angry Birds Classic 8.0.3 já espera para seus scripts.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
NEXTOS_ROOT=${NEXTOS_ROOT:?set NEXTOS_ROOT to a NextOS Elite build tree}
TOOLCHAIN=$(ls -d "$NEXTOS_ROOT"/build*Amlogic-old*/toolchain 2>/dev/null |
  sort -V | tail -1)
[[ -n ${TOOLCHAIN:-} ]] || {
  echo "toolchain Amlogic-old nao encontrada em $NEXTOS_ROOT" >&2
  exit 1
}
SYSROOT=$TOOLCHAIN/armv8a-emuelec-linux-gnueabihf/sysroot
LUAC=${AB_LUAC_ARM:-}
[[ -n $LUAC && -x $LUAC ]] || {
  echo "defina AB_LUAC_ARM para um luac 5.1 ARM de lua_Number=float" >&2
  exit 1
}

BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ab-lua-adapter.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT

QEMU_ARM=(qemu-arm -L "$SYSROOT")
if [[ ! -e $SYSROOT/lib/ld-linux-armhf.so.3 &&
      -e $SYSROOT/usr/lib/ld-linux-armhf.so.3 ]]; then
  # Current NextOS sysroots keep the ARMHF interpreter in usr/lib while the
  # guest ELF requests /lib. Invoke it directly instead of mutating sysroot.
  QEMU_ARM=(qemu-arm "$SYSROOT/usr/lib/ld-linux-armhf.so.3"
    --library-path "$SYSROOT/usr/lib")
fi

"${QEMU_ARM[@]}" "$LUAC" -s \
  -o "$BUILD_DIR/adapter.luac" "$PORT_DIR/src/ab_slingshot_adapter.lua"
xz --format=lzma --lzma1=dict=64KiB --stdout \
  "$BUILD_DIR/adapter.luac" >"$BUILD_DIR/adapter.lzma"
printf '\211LZMA\r\n\032\n' >"$BUILD_DIR/adapter.packed"
dd if="$BUILD_DIR/adapter.lzma" of="$BUILD_DIR/adapter.packed" \
  oflag=append conv=notrunc status=none
openssl enc -aes-256-cbc \
  -K 55534361505170413454534E56784D49317639534B39554330795A75416E6232 \
  -iv 00000000000000000000000000000000 \
  -in "$BUILD_DIR/adapter.packed" -out "$BUILD_DIR/adapter.enc"

{
  printf '%s\n' \
    '/* Generated from ab_slingshot_adapter.lua: Lua 5.1 bytecode, LZMA and the' \
    ' * owner'"'"'s APK-native AES envelope. It contains only the GPL NextOS adapter,' \
    ' * never Rovio code or game data. Regenerate with tools/build-lua-adapter.sh. */'
  xxd -i -n ab_slingshot_adapter_embedded "$BUILD_DIR/adapter.enc" |
    sed -e 's/^unsigned char/static const unsigned char/' \
        -e 's/^unsigned int/static const unsigned int/'
} >"$BUILD_DIR/ab_slingshot_adapter.inc"

if cmp -s "$BUILD_DIR/ab_slingshot_adapter.inc" \
    "$PORT_DIR/src/ab_slingshot_adapter.inc"; then
  echo "adapter ja esta atualizado"
else
  echo "artefato gerado em $BUILD_DIR/ab_slingshot_adapter.inc" >&2
  echo "revise e aplique-o a src/ab_slingshot_adapter.inc" >&2
  trap - EXIT
  exit 2
fi
