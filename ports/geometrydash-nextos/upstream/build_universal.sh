#!/usr/bin/env bash
# Build UNIVERSAL AArch64 de um dos dois jogos deste repo.
#
#   ./build_universal.sh geometrydash
#   ./build_universal.sh gdsubzero
#
# O toolchain cruzado do Debian Buster mantem o executavel em GLIBC <= 2.30,
# que e' o teto do pacote publico (suportando_outros_devices/runtime-abi-glibc.md).
# SDL2, GLESv2, EGL e FreeType sao do FIRMWARE do aparelho: entram so' como
# stubs que gravam o SONAME certo, nunca as libs de um sysroot -- assim o ELF
# nao herda a glibc de quem compilou. Headers vem de um sysroot montado
# somente-leitura, e header nao define ABI de libc.
#
# Fontes: `src/` e' o nucleo COMUM aos dois jogos (byte a byte); o que diverge
# de verdade mora em `games/<jogo>/src/` e vence o arquivo de mesmo nome.
set -euo pipefail

GAME=${1:-}
case "$GAME" in
  geometrydash) GAME_TITLE="Geometry Dash" ;;
  gdsubzero)    GAME_TITLE="Geometry Dash SubZero" ;;
  *) echo "uso: $0 {geometrydash|gdsubzero}" >&2; exit 2 ;;
esac

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${GD_UNIVERSAL_OUTPUT:-$GAME-universal}

if [ "${GD_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  SYSROOT=${GD_HEADER_SYSROOT:-}
  if [ -z "$SYSROOT" ]; then
    # Qualquer sysroot AArch64 com os headers de SDL2/GLES/EGL/FreeType serve:
    # header nao define ABI de libc, e as libs entram como stub. Aponte
    # GD_HEADER_SYSROOT para o seu, ou NEXTOS_ROOT para uma arvore do NextOS.
    for root in "${NEXTOS_ROOT:-$HOME/NextOS-Elite-Edition}"; do
      [ -d "$root" ] || continue
      tc=$(find -H "$root" -maxdepth 2 -type d \
            -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
            -print 2>/dev/null | sort -V | tail -1)
      [ -n "$tc" ] || continue
      [ -d "$tc/aarch64-libreelec-linux-gnu/sysroot" ] || continue
      SYSROOT=$tc/aarch64-libreelec-linux-gnu/sysroot
      break
    done
  fi
  [ -n "$SYSROOT" ] ||
    { echo "sysroot de HEADERS nao encontrado (defina GD_HEADER_SYSROOT)" >&2; exit 1; }
  command -v docker >/dev/null 2>&1 ||
    { echo "docker e' necessario para a build GLIBC <= 2.30" >&2; exit 1; }

  exec docker run --rm \
    -e GD_BUSTER_IN_CONTAINER=1 \
    -e GD_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e GD_HOST_UID="$(id -u)" \
    -e GD_HOST_GID="$(id -g)" \
    -v "$PORT_DIR":/repo \
    -v "$SYSROOT":/nxsr:ro \
    debian:buster \
    bash /repo/build_universal.sh "$GAME"
fi

export DEBIAN_FRONTEND=noninteractive
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  printf '%s\n' \
    'deb http://archive.debian.org/debian buster main' \
    'deb http://archive.debian.org/debian-security buster/updates main' \
    > /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    libc6-dev-arm64-cross file >/dev/null
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd /repo

# Caminhos FIXOS, nao mktemp: o nome do diretorio temporario entra no simbolo
# de arquivo do objeto do .S e vazava para o binario, de modo que duas builds
# da mesma fonte saiam com sha256 diferentes. Aqui dentro do container isso e'
# seguro, e o binario publicado passa a ser conferivel contra a fonte.
OBJDIR=/tmp/gd-build-obj
STUBDIR=/tmp/gd-build-stub
rm -rf "$OBJDIR" "$STUBDIR"
mkdir -p "$OBJDIR" "$STUBDIR"
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

# O overlay do jogo vence o nucleo no arquivo de mesmo nome. `imports.gen.c` e'
# gerado e nunca entra no link.
SRCS=""
for source in "games/$GAME/src"/*.c "games/$GAME/src"/*.S; do
  [ -e "$source" ] || continue
  SRCS="$SRCS $source"
done
for source in src/*.c src/*.S; do
  [ -e "$source" ] || continue
  base=$(basename "$source")
  [ "$base" = "imports.gen.c" ] && continue
  [ -e "games/$GAME/src/$base" ] && continue
  SRCS="$SRCS $source"
done

OBJS=()
for source in $SRCS; do
  object="$OBJDIR/$(basename "${source%.*}").o"
  "$CC" -D_GNU_SOURCE -DGD_TITLE="\"$GAME_TITLE\"" -I src -I "games/$GAME/src" \
    -idirafter /nxsr/usr/include \
    -idirafter /nxsr/usr/include/SDL2 \
    -idirafter /nxsr/usr/include/freetype2 \
    -O2 -g -fPIC -fno-omit-frame-pointer \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# ---- stubs de link: gravam o SONAME certo sem importar a glibc de ninguem ----
UNDEFINED=$("$NM" --undefined-only "${OBJS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)

stub_lib() {
  stub_name=$1; stub_soname=$2; stub_regex=$3
  : > "$STUBDIR/$stub_name.c"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$stub_regex" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/$stub_name.c"
  done
  "$CC" -shared -fPIC -nostdlib -Wl,-soname,"$stub_soname" \
    "$STUBDIR/$stub_name.c" -o "$STUBDIR/lib$stub_name.so"
}
stub_lib SDL2     libSDL2-2.0.so.0 '^SDL_'
stub_lib GLESv2   libGLESv2.so.2   '^gl[A-Z]'
stub_lib EGL      libEGL.so.1      '^egl[A-Z]'
stub_lib freetype libfreetype.so.6 '^FT_'

"$CC" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lgcc_s

# ---- trava de portabilidade: GLIBC <= 2.30 ----
MAX_GLIBC=$(
  "$READELF" --version-info "$OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1
)
[ -n "$MAX_GLIBC" ] ||
  { echo "nao foi possivel determinar a versao GLIBC de $OUTPUT" >&2; exit 1; }
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]; }; then
  echo "FALHA: $OUTPUT exige $MAX_GLIBC (limite GLIBC_2.30)" >&2
  exit 1
fi

# ---- nenhum DT_NEEDED alem do que o firmware garante ----
NEEDED=$("$READELF" -dW "$OUTPUT" | awk -F'[][]' '/NEEDED/ {print $2}' | sort)
EXPECTED=$(printf '%s\n' libSDL2-2.0.so.0 libGLESv2.so.2 libEGL.so.1 \
  libfreetype.so.6 libdl.so.2 libm.so.6 libpthread.so.0 libgcc_s.so.1 \
  libc.so.6 | sort)
if [ "$NEEDED" != "$EXPECTED" ]; then
  echo "FALHA: DT_NEEDED inesperado:" >&2
  printf '  obtido:   %s\n' "$(echo "$NEEDED" | tr '\n' ' ')" >&2
  printf '  esperado: %s\n' "$(echo "$EXPECTED" | tr '\n' ' ')" >&2
  exit 1
fi

if [ -n "${GD_HOST_UID:-}" ] && [ -n "${GD_HOST_GID:-}" ]; then
  chown "$GD_HOST_UID:$GD_HOST_GID" "$OUTPUT" 2>/dev/null || true
fi

echo "UNIVERSAL AARCH64 BUILD OK -> $OUTPUT"
echo "glibc maxima: $MAX_GLIBC (limite: GLIBC_2.30)"
echo "DT_NEEDED: $(echo "$NEEDED" | tr '\n' ' ')"
file "$OUTPUT"
sha256sum "$OUTPUT"
