#!/usr/bin/env bash
# Build the last-resort FreeType/HarfBuzz fallback pair shipped inside the port.
#
# Firmware libraries always win: the starter only falls back to these when the CFW
# has no usable libfreetype.so.6 / libharfbuzz.so.0 anywhere it can see.  They are
# therefore built to be dependency-free beyond libc/libm: no zlib, png, bzip2,
# brotli, glib, graphite2, icu or cairo, and libstdc++ is linked statically.
#
# HarfBuzz keeps FreeType support on purpose: the game's bindings import
# hb_ft_font_create_referenced, which does not exist in a --without-freetype build.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

PORT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
OUT_DIR=${1:-"$PORT_DIR/build/fallback-libs"}
IMAGE=${SOR4_FALLBACK_IMAGE:-sor4-fallback-builder:debian-buster-v2}
FREETYPE_VERSION=2.10.4
HARFBUZZ_VERSION=2.6.4
FREETYPE_SHA256=86a854d8905b19698bbc8f23b860bc104246ce4854dcea8e3b0fb21284f75784
HARFBUZZ_SHA256=9413b8d96132d699687ef914ebb8c50440efc87b3f775d25856d7ec347c03c12

fail() {
    printf 'fallback libs error: %s\n' "$*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail "docker is required for a reproducible buster build"
mkdir -p -- "$OUT_DIR"
OUT_DIR=$(cd -- "$OUT_DIR" && pwd -P)

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    printf '[fallback libs] preparing %s\n' "$IMAGE"
    docker build -t "$IMAGE" - <<'DOCKERFILE'
FROM debian:buster
RUN sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g' \
        /etc/apt/sources.list \
    && sed -i '/buster-updates/d' /etc/apt/sources.list \
    && printf 'Acquire::Check-Valid-Until "false";\n' > /etc/apt/apt.conf.d/99no-check-valid \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
        gcc g++ make pkg-config ca-certificates curl xz-utils file binutils \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi

docker run --rm -v "$OUT_DIR:/out" "$IMAGE" bash -euo pipefail -c "
export LC_ALL=C TZ=UTC
cd /tmp
curl -fsSLo freetype.tar.xz 'https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.xz'
echo '$FREETYPE_SHA256  freetype.tar.xz' | sha256sum -c -
curl -fsSLo harfbuzz.tar.xz 'https://github.com/harfbuzz/harfbuzz/releases/download/$HARFBUZZ_VERSION/harfbuzz-$HARFBUZZ_VERSION.tar.xz'
echo '$HARFBUZZ_SHA256  harfbuzz.tar.xz' | sha256sum -c -
tar xf freetype.tar.xz
tar xf harfbuzz.tar.xz

PREFIX=/tmp/stage
mkdir -p \"\$PREFIX\"

cd /tmp/freetype-$FREETYPE_VERSION
./configure --host=aarch64-linux-gnu --prefix=\"\$PREFIX\" \
    --enable-shared --disable-static \
    --with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no --with-brotli=no \
    CFLAGS='-O2 -fPIC' >/dev/null
make -j\"\$(nproc)\" >/dev/null
make install >/dev/null

cd /tmp/harfbuzz-$HARFBUZZ_VERSION
PKG_CONFIG_PATH=\"\$PREFIX/lib/pkgconfig\" PKG_CONFIG_LIBDIR=\"\$PREFIX/lib/pkgconfig\" \
./configure --host=aarch64-linux-gnu --prefix=\"\$PREFIX\" \
    --enable-shared --disable-static \
    --with-freetype=yes --with-glib=no --with-gobject=no --with-cairo=no \
    --with-icu=no --with-graphite2=no --with-fontconfig=no \
    CXXFLAGS='-O2 -fPIC' LDFLAGS='-static-libstdc++ -static-libgcc' >/dev/null
make -j\"\$(nproc)\" >/dev/null
make install >/dev/null

cd /out
rm -f libfreetype.so.6 libharfbuzz.so.0
cp \"\$(readlink -f \$PREFIX/lib/libfreetype.so.6)\" libfreetype.so.6
cp \"\$(readlink -f \$PREFIX/lib/libharfbuzz.so.0)\" libharfbuzz.so.0
aarch64-linux-gnu-strip libfreetype.so.6 libharfbuzz.so.0
chmod 644 libfreetype.so.6 libharfbuzz.so.0
"

for library in "$OUT_DIR/libfreetype.so.6" "$OUT_DIR/libharfbuzz.so.0"; do
    [[ -f "$library" ]] || fail "missing build output: $library"
    machine=$(readelf -h "$library" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
    [[ "$machine" = AArch64 ]] || fail "$library is not AArch64"
    newest=$(readelf --version-info "$library" 2>/dev/null |
        sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
    if [[ -n "$newest" && "$(printf '%s\n%s\n' 2.17 "$newest" | sort -V | tail -n 1)" != 2.17 ]]; then
        fail "$library requires GLIBC_$newest (maximum allowed is 2.17)"
    fi
    # A fallback that needs a library the lean firmware also lacks is not a fallback.
    while read -r needed; do
        case "$needed" in
            libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|libfreetype.so.6) ;;
            *) fail "$library pulls an unexpected dependency: $needed" ;;
        esac
    done < <(readelf -d "$library" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
done

# Captured first on purpose: with pipefail, grep -q closing the pipe early would make
# readelf fail and the check would report a false negative.
harfbuzz_symbols=$(readelf --dyn-syms -W "$OUT_DIR/libharfbuzz.so.0")
grep -qw 'hb_ft_font_create_referenced' <<<"$harfbuzz_symbols" ||
    fail "bundled HarfBuzz has no FreeType integration"

# Stage them where the package builder expects them, clearly separated from the
# bundle's own libraries so nobody mistakes a last resort for a preferred library.
STAGE_DIR="$PORT_DIR/build/host_pkg/libs/fallback"
if [[ -d "$PORT_DIR/build/host_pkg" ]]; then
    install -D -m 0644 -- "$OUT_DIR/libfreetype.so.6" "$STAGE_DIR/libfreetype.so.6"
    install -D -m 0644 -- "$OUT_DIR/libharfbuzz.so.0" "$STAGE_DIR/libharfbuzz.so.0"
    printf 'fallback libs staged: %s\n' "$STAGE_DIR"
fi

printf 'fallback libs: %s\n' "$OUT_DIR"
ls -l "$OUT_DIR"
