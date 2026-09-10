#!/bin/bash
set -euo pipefail
umask 022

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
OUTPUT=${1:-"$ROOT/../forager-nextos"}
UPSTREAM=https://github.com/JohnnyonFlame/gmloader-next.git
COMMIT=c2fca354df73761887c15f44a0b28ec823581cd5
IMAGE=forager-armhf-builder:buster
BUILD_DATE=20260813_000000

command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
command -v git >/dev/null || { echo "git is required" >&2; exit 1; }

if [ "${FORAGER_REBUILD_IMAGE:-0}" = 1 ] || \
   ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" -f "$ROOT/Dockerfile.armhf-buster" "$ROOT"
fi

(cd "$ROOT/overlay" && sha256sum -c ../overlay.sha256)

stage=$(mktemp -d)
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT

git -c init.defaultBranch=source init "$stage/gmloader-next"
git -C "$stage/gmloader-next" remote add origin "$UPSTREAM"
git -C "$stage/gmloader-next" fetch --depth=1 origin "$COMMIT"
git -C "$stage/gmloader-next" checkout --detach FETCH_HEAD
git -C "$stage/gmloader-next" submodule update --init \
  3rdparty/json 3rdparty/libbsd 3rdparty/libmd 3rdparty/libzip

cp -a "$ROOT/overlay/." "$stage/gmloader-next/"
install -D -m 0755 "$ROOT/arm-linux-gnueabihf-pkg-config" \
  "$stage/gmloader-next/.pcwrap/arm-linux-gnueabihf-pkg-config"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e SOURCE_DATE_EPOCH=1786579200 \
  -v "$stage/gmloader-next:/src" \
  -w /src "$IMAGE" bash -lc \
  'set -e; \
   export PATH=/src/.pcwrap:$PATH; \
   make -f Makefile.gmloader \
     ARCH=arm-linux-gnueabihf CROSS=arm-linux-gnueabihf- clean; \
   python3 scripts/generate_libc.py arm-linux-gnueabihf \
     --llvm-library-file /usr/lib/llvm-11/lib/libclang.so \
     --llvm-includes \
       /usr/arm-linux-gnueabihf/include \
       /usr/arm-linux-gnueabihf/include/c++/8 \
       /usr/arm-linux-gnueabihf/include/c++/8/arm-linux-gnueabihf \
       /usr/lib/llvm-11/lib/clang/11.0.1/include; \
   make -f Makefile.gmloader \
     ARCH=arm-linux-gnueabihf CROSS=arm-linux-gnueabihf- \
     LLVM_FILE=/usr/lib/llvm-11/lib/libclang.so \
     LLVM_INC="/usr/arm-linux-gnueabihf/include /usr/arm-linux-gnueabihf/include/c++/8 /usr/arm-linux-gnueabihf/include/c++/8/arm-linux-gnueabihf /usr/lib/llvm-11/lib/clang/11.0.1/include" \
     STATIC_LIBSTDCXX=1 BUILD_DATE=20260813_000000 \
     GITHUB_SHA=c2fca354df73761887c15f44a0b28ec823581cd5 \
     GITHUB_REF_NAME=forager_1_0_0 -j"$(nproc)"; \
   arm-linux-gnueabihf-strip \
     -o build/arm-linux-gnueabihf/gmloader/forager-nextos \
     build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf'

install -D -m 0755 \
  "$stage/gmloader-next/build/arm-linux-gnueabihf/gmloader/forager-nextos" \
  "$OUTPUT"
sha256sum "$OUTPUT"
file "$OUTPUT"
