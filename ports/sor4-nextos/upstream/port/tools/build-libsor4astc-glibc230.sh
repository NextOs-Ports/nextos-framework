#!/bin/bash
# Compila libsor4astc.so (sor4_astc_decode + sor4_etc1_encode) com GLIBC <= 2.30
# (Debian Buster ARM64 = glibc 2.28; símbolos resultantes <= GLIBC_2.27, GLIBCXX_3.4.11,
# CXXABI_1.3.9) -> roda em devices glibc 2.30 E 2.43 (universal).
# Requer Docker + binfmt arm64: docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# Decoder ASTC = astcenc 5.0.0 (decompress-only). Encoder ETC1 = nosso etc1_encode.c.
# IMPORTANTE: etc1_encode.c compila com GCC (C, símbolo etc1_encode_image SEM mangling);
# sor4astc.c o inclui dentro de extern "C". Compilar etc1 com g++ quebra (símbolo manglado).
#
# Uso: ASTCENC5_SRC=/caminho/astc-encoder/Source ./build-libsor4astc-glibc230.sh
#   (astcenc 5.0.0 -> context_alloc com 3 args; 5.4.0 NAO serve, API mudou p/ 4 args)
set -euo pipefail
cd "$(dirname "$0")/../.."        # ports/sor4 (script vive em ports/sor4/port/tools/)
SRC="$(pwd)/build"               # sor4astc.c, etc1_encode.c, etc1_encode.h, eac_encode.c
ASTCENC5_SRC="${ASTCENC5_SRC:-}"
[ -n "$ASTCENC5_SRC" ] && [ -d "$ASTCENC5_SRC" ] || {
  echo "ERRO: defina ASTCENC5_SRC para o diretorio Source do Arm astcenc 5.0.0" >&2
  exit 2
}
[ -f "$ASTCENC5_SRC/astcenc.h" ] || {
  echo "ERRO: ASTCENC5_SRC nao contem astcenc.h" >&2
  exit 2
}

S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT INT TERM
mkdir -p "$S/Source"
cp "$ASTCENC5_SRC"/*.cpp "$ASTCENC5_SRC"/*.h "$S/Source/"
cp "$SRC/sor4astc.c" "$SRC/etc1_encode.c" "$SRC/etc1_encode.h" "$SRC/eac_encode.c" "$SRC/eac_encode.h" "$S/"

docker run --rm --platform linux/arm64 -v "$S":/work -w /work debian:buster bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
printf "deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian-security buster/updates main\n" > /etc/apt/sources.list
apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null 2>&1
apt-get install -y -qq --no-install-recommends g++ gcc >/dev/null 2>&1
gcc -O2 -fPIC -c etc1_encode.c -o etc1_encode.o
gcc -O2 -fPIC -c eac_encode.c -o eac_encode.o
g++ -O2 -fPIC -shared -std=c++17 -DASTCENC_DECOMPRESS_ONLY=1 -fno-strict-aliasing -I Source -I . \
    -o libsor4astc.so Source/astcenc_*.cpp sor4astc.c etc1_encode.o eac_encode.o -lm
strip libsor4astc.so
echo "libsor4astc.so $(stat -c%s libsor4astc.so) bytes"
'
cp "$S/libsor4astc.so" "$SRC/host_pkg/libs/libsor4astc.so"
echo "OK -> build/host_pkg/libs/libsor4astc.so (deploy no device + zip)"
