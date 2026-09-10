#!/usr/bin/env bash
# Reproducible ARMHF public build. Debian Buster supplies glibc/libgcc while
# the NextOS sysroot supplies architecture-neutral SDL/EGL/GLES headers only.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=build/titansouls-nextos
BUILDER_IMAGE=codboz-armhf-builder:debian-buster
BUILDER_IMAGE_ID=sha256:58e229c82e8270fd69b0c307654e31793b781218884ce868844597ec4b8c9fef
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786406400}

if [ "${TS_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  REPOSITORY_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
  FRAMEWORK_HOST_ROOT=${NEXTOS_FRAMEWORK_ROOT:-}
  if [ -z "$FRAMEWORK_HOST_ROOT" ] &&
      [ -d "$REPOSITORY_ROOT/framework" ]; then
    FRAMEWORK_HOST_ROOT=$REPOSITORY_ROOT/framework
  fi
  [ -n "$FRAMEWORK_HOST_ROOT" ] && [ -d "$FRAMEWORK_HOST_ROOT" ] || {
    echo "set NEXTOS_FRAMEWORK_ROOT to the external NextOS framework source tree" >&2
    exit 1
  }
  FRAMEWORK_HOST_ROOT=$(CDPATH= cd -- "$FRAMEWORK_HOST_ROOT" && pwd)

  [ -n "${NEXTOS_SDK_ROOT:-}" ] && [ -d "$NEXTOS_SDK_ROOT" ] || {
    echo "set NEXTOS_SDK_ROOT to a NextOS SDK tree or its ARMHF sysroot" >&2
    exit 1
  }
  SDK_INPUT_ROOT=$(CDPATH= cd -- "$NEXTOS_SDK_ROOT" && pwd)
  if [ -d "$SDK_INPUT_ROOT/usr/include/SDL2" ]; then
    NEXTOS_SYSROOT=$SDK_INPUT_ROOT
  else
    NEXTOS_TOOLCHAIN=$(
      find -H "$SDK_INPUT_ROOT" -maxdepth 2 -type d \
        -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
        -print | sort -V | tail -1
    )
    [ -n "$NEXTOS_TOOLCHAIN" ] || {
      echo "ARMHF header sysroot not found below NEXTOS_SDK_ROOT" >&2
      exit 1
    }
    NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/armv8a-emuelec-linux-gnueabihf/sysroot
  fi
  [ -d "$NEXTOS_SYSROOT/usr/include/SDL2" ] || {
    echo "NEXTOS_SDK_ROOT does not provide ARMHF SDL2 headers" >&2
    exit 1
  }
  command -v docker >/dev/null 2>&1 || {
    echo "docker is required for the pinned low-glibc build" >&2
    exit 1
  }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" \
    --format '{{.Id}}' 2>/dev/null) || {
      echo "offline builder image missing: $BUILDER_IMAGE" >&2
      exit 1
    }
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "builder image changed: $ACTUAL_IMAGE_ID" >&2
    exit 1
  }
  exec docker run --rm --network none \
    -e TS_BUSTER_IN_CONTAINER=1 \
    -e TS_HOST_UID="$(id -u)" -e TS_HOST_GID="$(id -g)" \
    -e LC_ALL=C -e TZ=UTC -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -e NEXTOS_FRAMEWORK_ROOT=/framework -e NEXTOS_SDK_ROOT=/nxsr \
    -v "$PORT_DIR":/repo \
    -v "$FRAMEWORK_HOST_ROOT":/framework:ro \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUILDER_IMAGE_ID" bash /repo/build_universal.sh
fi

for tool in arm-linux-gnueabihf-gcc arm-linux-gnueabihf-nm \
            arm-linux-gnueabihf-readelf arm-linux-gnueabihf-strings \
            arm-linux-gnueabihf-strip file; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing pinned-builder tool: $tool" >&2
    exit 1
  }
done

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
READELF=arm-linux-gnueabihf-readelf
STRINGS=arm-linux-gnueabihf-strings
STRIP=arm-linux-gnueabihf-strip
FRAMEWORK_ROOT=${NEXTOS_FRAMEWORK_ROOT:?NEXTOS_FRAMEWORK_ROOT is required}
SDK_SYSROOT=${NEXTOS_SDK_ROOT:?NEXTOS_SDK_ROOT is required}
cd /repo
mkdir -p build

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
cleanup() {
  find "$OBJDIR" "$STUBDIR" -type f -delete 2>/dev/null || true
  rmdir "$OBJDIR" "$STUBDIR" 2>/dev/null || true
}
trap cleanup EXIT

COMMON_INCLUDES=(
  -I src
  -I "$FRAMEWORK_ROOT/nxloader/include"
  -I "$FRAMEWORK_ROOT/nxloader/src"
  -I "$FRAMEWORK_ROOT/nxandroid/include"
  -I "$FRAMEWORK_ROOT/nxcompat/include"
  -I "$FRAMEWORK_ROOT/nxcompat/src"
  -I "$FRAMEWORK_ROOT/nxgl/include"
  -I "$FRAMEWORK_ROOT/nxgl/src"
  -I "$FRAMEWORK_ROOT/nxinput/include"
  -I "$FRAMEWORK_ROOT/nxinput/src"
  -I "$FRAMEWORK_ROOT/nxaudio/include"
)

OBJS=()
compile_source() {
  group=$1
  source=$2
  object="$OBJDIR/${group}_$(basename "${source%.*}").o"
  "$CC" -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    "${COMMON_INCLUDES[@]}" \
    -idirafter "$SDK_SYSROOT/usr/include/SDL2" \
    -idirafter "$SDK_SYSROOT/usr/include" \
    -O2 -fPIC -fno-omit-frame-pointer -fno-strict-aliasing \
    -fno-builtin-powf -fno-builtin-expf -fno-builtin-exp2f \
    -fno-builtin-logf -fno-builtin-log2f \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-int-conversion \
    -Wno-incompatible-pointer-types -Wno-format-truncation \
    -c "$source" -o "$object"
  OBJS+=("$object")
}

PORT_SOURCES=(
  src/main.c src/imports.c src/audio_recovery_policy.c src/cpuinfo_compat.c
  src/util.c src/error.c
  src/bionic_compat.c src/softfp_bridge.c src/platform_shims.c
  src/pthread_bridge.c src/android_shim.c src/asset_shim.c
  src/egl_shim.c src/jni_shim.c src/language_menu.c
  src/language_menu_policy.c src/opensles_shim.c src/setjmp_bridge.S
  src/ts_loader.c src/loader_compat.c src/framework_bridge.c src/lifecycle.c
)
for source in "${PORT_SOURCES[@]}"; do
  compile_source port "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_elf32.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_elf64.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_hooks.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_protect.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_registry.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_softfp.c; do
  compile_source nxloader "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxandroid/src/nxandroid.c \
  "$FRAMEWORK_ROOT"/nxandroid/src/nxandroid_imports.c; do
  compile_source nxandroid "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_backend.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_graphics.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_plan.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_probe.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_receipts.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_registry.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_report.c; do
  compile_source nxcompat "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_arbiter.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_diagnostics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_logic.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_metrics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_present.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_sdl2.c; do
  compile_source nxgl "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_core.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_nxcompat.c; do
  compile_source nxinput "$source"
done

compile_source nxaudio "$FRAMEWORK_ROOT/nxaudio/src/nxaudio.c"

# Firmware libraries are selected at runtime. Generated link-only stubs keep
# newer NextOS libraries (and their glibc floor) out of the public executable.
UNDEFINED=$($NM --undefined-only "${OBJS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)
make_stub() {
  soname=$1
  pattern=$2
  output=$3
  source="$STUBDIR/$output.c"
  : > "$source"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$pattern" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$source"
  done
  "$CC" -shared -fPIC -nostdlib -Wl,-soname,"$soname" \
    "$source" -o "$STUBDIR/lib$output.so"
}
make_stub libSDL2-2.0.so.0 '^SDL_' SDL2
make_stub libEGL.so.1 '^egl' EGL
make_stub libGLESv2.so.2 '^gl[A-Z]' GLESv2

"$CC" -no-pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -Wl,--no-as-needed -lSDL2 -lEGL -lGLESv2 \
  -Wl,--as-needed -ldl -lm -lpthread -latomic -lgcc_s \
  -Wl,--build-id=sha1,-z,relro,-z,now,-z,noexecstack

MAX_GLIBC=$(
  "$READELF" --version-info "$OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1
)
[ -n "$MAX_GLIBC" ] || {
  echo "unable to determine the GLIBC requirement" >&2
  exit 1
}
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
if [ "$major" -gt 2 ] || {
  [ "$major" -eq 2 ] && [ "$minor" -gt 30 ];
}; then
  echo "public build rejected: $MAX_GLIBC exceeds GLIBC_2.30" >&2
  exit 1
fi

if "$READELF" -lW "$OUTPUT" |
    awk '$1 == "LOAD" && $0 ~ /RWE/ { bad=1 } END { exit !bad }'; then
  echo "public build contains an RWX PT_LOAD" >&2
  exit 1
fi
if "$READELF" -dW "$OUTPUT" | grep -Eq '\((RPATH|RUNPATH)\)'; then
  echo "public build contains a forbidden DT_RPATH/DT_RUNPATH" >&2
  exit 1
fi

# The final runtime may keep the narrow so_find_exidx compatibility symbol,
# but none of the legacy loader/mapping/permission entry points may survive.
if "$NM" "$OUTPUT" | awk \
    '$3 ~ /^(so_load|so_relocate|so_resolve|so_finalize|so_unload|hook_arm|so_make_text_writable|so_make_text_executable)$/ { found=1 } END { exit !found }'; then
  echo "public build still contains the legacy so_util loader" >&2
  exit 1
fi
for symbol in nxloader_module_load_file nxloader_module_finalize \
              nxloader_softfp_add_libm nxandroid_context_run \
              nxcompat_probe nxgl_open_v2 nxinput_create \
              nxaudio_adapter_validate ts_runtime_graphics_ready; do
  if ! "$NM" "$OUTPUT" | awk -v symbol="$symbol" \
      '$3 == symbol { found=1 } END { exit !found }'; then
    echo "required framework symbol missing from public build: $symbol" >&2
    exit 1
  fi
done
INTERPRETER=$(
  "$READELF" -lW "$OUTPUT" |
    sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p'
)
[ "$INTERPRETER" = "/lib/ld-linux-armhf.so.3" ] || {
  echo "unexpected ARMHF interpreter: $INTERPRETER" >&2
  exit 1
}

# Local ELF FILE symbols record the randomized temporary object directory.
# They are not used for runtime symbolization (the exported dynsym is), and
# keeping them would make otherwise identical builds differ byte-for-byte.
"$STRIP" --strip-unneeded "$OUTPUT"

LC_ALL=C "$STRINGS" "$OUTPUT" > "$OBJDIR/public-strings.txt"
BUILD_LEAK_PATTERN='/home/[A-Za-z0-9._-]+|/mnt/'"ARQUIVOS"'|192[.]168[.][0-9]'
if grep -Eq "$BUILD_LEAK_PATTERN" "$OBJDIR/public-strings.txt"; then
  echo "public build leaks a build-machine path or address" >&2
  exit 1
fi

chmod 0755 "$OUTPUT"
if [ -n "${TS_HOST_UID:-}" ] && [ -n "${TS_HOST_GID:-}" ]; then
  chown "$TS_HOST_UID:$TS_HOST_GID" "$OUTPUT"
fi
echo "Titan Souls public ARMHF build OK: $OUTPUT ($MAX_GLIBC)"
file "$OUTPUT"
