#!/bin/sh
# Compila o FDK-AAC (somente decodificador) como libfdk-aac-dec.a aarch64.
# Rodado DENTRO do container buster. Nao usa autotools/cmake de proposito:
# a arvore e' um conjunto plano de .cpp e a dependencia extra so aumentaria
# a superficie do build reproduzivel.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"/fdk-aac-2.0.3
OUT="$(cd "$(dirname "$0")" && pwd)"/build
mkdir -p "$OUT"
cd "$OUT"

DEC_LIBS="libAACdec libFDK libMpegTPDec libPCMutils libSBRdec libSYS libArithCoding libDRCdec libSACdec"

INC=""
for l in $DEC_LIBS; do
  INC="$INC -I$SRC/$l/include -I$SRC/$l/src"
done

CXX=aarch64-linux-gnu-g++-8
CXXFLAGS="-O2 -fPIC -fno-exceptions -fno-rtti -DHAVE_STDINT_H=1 -w $INC"

n=0
for l in $DEC_LIBS; do
  for f in "$SRC/$l/src"/*.cpp; do
    [ -f "$f" ] || continue
    o="$OUT/$(echo "$l-$(basename "$f" .cpp)").o"
    $CXX $CXXFLAGS -c "$f" -o "$o"
    n=$((n+1))
  done
done
echo "objetos compilados: $n"
aarch64-linux-gnu-ar rcs "$OUT/libfdk-aac-dec.a" "$OUT"/*.o
aarch64-linux-gnu-ranlib "$OUT/libfdk-aac-dec.a"
ls -l "$OUT/libfdk-aac-dec.a"
