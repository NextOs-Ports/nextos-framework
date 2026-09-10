#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Build universal aarch64 do Off The Road 1.18.2 (engine xGen/Horde3D+bgfx
# sobre casca Cocos2d-x).
#
# Molde: ports/chrono/build_universal.sh + ports/ff4/build_universal.sh
# (ambos aprovados; ff4 = onda v2, nxgl 0.2.14). O toolchain cruzado do
# Debian Buster mantem o executavel em GLIBC <= 2.30, teto do pacote publico.
# SDL2 e GLESv2 sao do FIRMWARE do aparelho: entram so como stubs
# que gravam o SONAME certo, nunca as libs do sysroot NextOS. O sysroot
# NextOS entra somente-leitura e so' por HEADERS.
#
# EGL NAO vira DT_NEEDED. eglGetProcAddress segue por SDL_GL_GetProcAddress;
# os outros 14 imports do guest sao ligados explicitamente, em runtime, ao
# provider que observa o contexto SDL corrente. Em GLVND/ROCKNIX o adapter
# promove a libEGL ja carregada de RTLD_LOCAL para RTLD_GLOBAL via dlopen,
# seguindo o padrao comprovado por Merchant/Horizon Chase. O egl_shim.c antigo da
# linhagem bully continua fora: ele criaria outro contexto e quebraria a
# adocao nativa feita pelo bgfx.
#
# Uso no host:  ./build_universal.sh
# Public-final: nxrelease define NX_PUBLIC_FINAL_OUTPUT_DIR; o build monta esse
# diretório externo no container e grava somente offtheroad-nextos nele.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${OT_UNIVERSAL_OUTPUT:-offtheroad-nextos}
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
FRAMEWORK_PIN=$PORT_DIR/FRAMEWORK-PIN.json
BUILD_INPUTS=$PORT_DIR/tools/BUILD-INPUTS.json
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

# The public executable name is part of the port identity.  Allow an absolute
# output directory for the isolated nxrelease build, but never accept a legacy
# or ad-hoc executable basename.
case "$OUTPUT" in
  offtheroad-nextos|*/offtheroad-nextos) ;;
  *) echo "nome de executavel invalido: esperado offtheroad-nextos" >&2; exit 1 ;;
esac

if [ "${OT_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  REPOSITORY_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
  PIN_TOOL=$REPOSITORY_ROOT/framework/nxgenerator/framework_pin.py
  [ -f "$FRAMEWORK_PIN" ] && [ -f "$BUILD_INPUTS" ] && [ -f "$PIN_TOOL" ] ||
    { echo "pins canonicos de build ausentes" >&2; exit 1; }
  EXPECTED_FRAMEWORK_PIN=$(python3 -B -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["framework_pin_sha256"])' \
    "$BUILD_INPUTS")
  ACTUAL_FRAMEWORK_PIN=$(sha256sum "$FRAMEWORK_PIN" | awk '{print $1}')
  [ "$ACTUAL_FRAMEWORK_PIN" = "$EXPECTED_FRAMEWORK_PIN" ] || {
    echo "FRAMEWORK-PIN.json divergiu do BUILD-INPUTS.json" >&2
    exit 1
  }
  EXPECTED_IMAGE=$(python3 -B -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["builder"]["image"])' \
    "$BUILD_INPUTS")
  EXPECTED_IMAGE_ID=$(python3 -B -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["builder"]["image_id"])' \
    "$BUILD_INPUTS")
  [ "$BUILDER_IMAGE" = "$EXPECTED_IMAGE" ] &&
    [ "$BUILDER_IMAGE_ID" = "$EXPECTED_IMAGE_ID" ] || {
      echo "imagem declarada divergiu do BUILD-INPUTS.json" >&2
      exit 1
    }
  PIN_STAGE=$(mktemp -d "${TMPDIR:-/tmp}/otr-framework-pin.XXXXXX")
  trap 'rm -rf -- "$PIN_STAGE"' EXIT INT TERM
  PIN_SNAPSHOT=$PIN_STAGE/snapshot
  python3 -B "$PIN_TOOL" materialize --repository "$REPOSITORY_ROOT" \
    --pin "$FRAMEWORK_PIN" --destination "$PIN_SNAPSHOT"
  python3 -B "$PIN_TOOL" verify --pin "$FRAMEWORK_PIN" \
    --snapshot "$PIN_SNAPSHOT"
  FRAMEWORK_SOURCE=$PIN_SNAPSHOT/framework

  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  NEXTOS_TOOLCHAIN=""
  EXPECTED_NEXTOS_IDENTITY=$(python3 -B -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["headers"]["nextos_sysroot"]["source_identity"])' \
    "$BUILD_INPUTS")
  EXPECTED_NEXTOS_HEADERS=$(python3 -B -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["headers"]["nextos_sysroot"]["tree_sha256"])' \
    "$BUILD_INPUTS")
  for candidate in $(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V -r
  ); do
    if [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/SDL2/SDL.h" ] &&
       [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/GLES2/gl2.h" ] &&
       [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/KHR/khrplatform.h" ] &&
       [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/freetype2/ft2build.h" ]; then
      candidate_identity=$(basename "$(dirname "$candidate")")
      [ "$candidate_identity" = "$EXPECTED_NEXTOS_IDENTITY" ] || continue
      candidate_sysroot=$candidate/aarch64-libreelec-linux-gnu/sysroot
      candidate_digest=$(python3 -B -c 'import hashlib,pathlib,sys
root=pathlib.Path(sys.argv[1]).resolve(); files=[]
for value in sys.argv[2:]:
 p=root/value
 files.extend([p] if p.is_file() else [x for x in p.rglob("*") if x.is_file() and not x.is_symlink()])
h=hashlib.sha256()
for p in sorted(set(files), key=lambda x:x.relative_to(root).as_posix()):
 rel=p.relative_to(root).as_posix(); digest=hashlib.sha256(p.read_bytes()).hexdigest()
 h.update((digest+"  "+rel+"\n").encode())
print(h.hexdigest())' "$candidate_sysroot" \
        usr/include/SDL2 usr/include/GLES2 usr/include/KHR usr/include/freetype2)
      if [ "$candidate_digest" = "$EXPECTED_NEXTOS_HEADERS" ]; then
        NEXTOS_TOOLCHAIN=$candidate
        break
      fi
    fi
  done
  [ -n "$NEXTOS_TOOLCHAIN" ] ||
    { echo "nenhum sysroot corresponde a identidade e ao hash pinados" >&2; exit 1; }
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
  command -v docker >/dev/null 2>&1 ||
    { echo "docker e' necessario para a build GLIBC <= 2.30" >&2; exit 1; }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" --format '{{.Id}}' 2>/dev/null) ||
    { echo "imagem offline ausente: $BUILDER_IMAGE" >&2; exit 1; }
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "imagem do builder mudou: $ACTUAL_IMAGE_ID (esperado $BUILDER_IMAGE_ID)" >&2
    exit 1
  }

  CONTAINER_OUTPUT=$OUTPUT
  PUBLIC_OUTPUT_MOUNT=()
  if [ "${NX_PUBLIC_FINAL_REPRO_BUILD:-0}" = "1" ]; then
    case "${NX_PUBLIC_FINAL_OUTPUT_DIR:-}" in
      /*) ;;
      *) echo "NX_PUBLIC_FINAL_OUTPUT_DIR must be an absolute directory" >&2; exit 1 ;;
    esac
    [ -d "$NX_PUBLIC_FINAL_OUTPUT_DIR" ] &&
      [ ! -L "$NX_PUBLIC_FINAL_OUTPUT_DIR" ] || {
        echo "NX_PUBLIC_FINAL_OUTPUT_DIR must be a real existing directory" >&2
        exit 1
      }
    [ -z "$(find "$NX_PUBLIC_FINAL_OUTPUT_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ] || {
      echo "NX_PUBLIC_FINAL_OUTPUT_DIR must start empty" >&2
      exit 1
    }
    mkdir -p -- "$NX_PUBLIC_FINAL_OUTPUT_DIR/offtheroad"
    PUBLIC_OUTPUT_MOUNT=(-v "$NX_PUBLIC_FINAL_OUTPUT_DIR:/nx-public-final-output")
    CONTAINER_OUTPUT=/nx-public-final-output/offtheroad/offtheroad-nextos
  fi

  docker run --rm --network none \
    "${PUBLIC_OUTPUT_MOUNT[@]}" \
    -e OT_BUSTER_IN_CONTAINER=1 \
    -e OT_UNIVERSAL_OUTPUT="$CONTAINER_OUTPUT" \
    -e OT_HOST_UID="$(id -u)" \
    -e OT_HOST_GID="$(id -g)" \
    -e LC_ALL=C -e TZ=UTC -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -v "$PORT_DIR":/repo \
    -v "$FRAMEWORK_SOURCE":/framework:ro \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUILDER_IMAGE_ID" \
    bash /repo/build_universal.sh
  exit $?
fi

for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-nm aarch64-linux-gnu-readelf; do
  command -v "$tool" >/dev/null 2>&1 ||
    { echo "ferramenta ausente na imagem fixada: $tool" >&2; exit 1; }
done

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
FRAMEWORK_ROOT=${OT_FRAMEWORK_ROOT:-/framework}
for component_version in nxgl:0.2.17 nxinput:0.5.1 nxaudio:0.3.1; do
  component=${component_version%%:*}
  expected=${component_version#*:}
  actual=$(tr -d '[:space:]' < "$FRAMEWORK_ROOT/$component/VERSION" 2>/dev/null || true)
  [ "$actual" = "$expected" ] || {
    echo "FALHA: framework RC exige $component $expected exato" >&2
    exit 1
  }
done
EXPECTED_NEXTOS_HEADERS=$(python3 -B -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["headers"]["nextos_sysroot"]["tree_sha256"])' \
  "$BUILD_INPUTS")
tree_digest() {
  python3 -B -c 'import hashlib,pathlib,sys
root=pathlib.Path(sys.argv[1]).resolve(); files=[]
for value in sys.argv[2:]:
 p=root/value
 files.extend([p] if p.is_file() else [x for x in p.rglob("*") if x.is_file() and not x.is_symlink()])
h=hashlib.sha256()
for p in sorted(set(files), key=lambda x:x.relative_to(root).as_posix()):
 rel=p.relative_to(root).as_posix(); digest=hashlib.sha256(p.read_bytes()).hexdigest()
 h.update((digest+"  "+rel+"\n").encode())
print(h.hexdigest())' "$@"
}
[ "$(tree_digest /nxsr usr/include/SDL2 usr/include/GLES2 usr/include/KHR usr/include/freetype2)" = \
  "$EXPECTED_NEXTOS_HEADERS" ] || {
  echo "FALHA: headers NextOS nao correspondem ao BUILD-INPUTS.json" >&2
  exit 1
}
EXPECTED_COMPILER=$(python3 -B -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["builder"]["compiler"])' \
  "$BUILD_INPUTS")
[ "$(aarch64-linux-gnu-gcc --version | head -1)" = "$EXPECTED_COMPILER" ] || {
  echo "FALHA: compilador divergiu do BUILD-INPUTS.json" >&2
  exit 1
}
cd /repo

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

COMMON_INCLUDES=(
  -I src
  -I "$FRAMEWORK_ROOT/nxgl/include"
  -I "$FRAMEWORK_ROOT/nxgl/src"
  -I "$FRAMEWORK_ROOT/nxgl/adapters"
  -I "$FRAMEWORK_ROOT/nxinput/include"
  -I "$FRAMEWORK_ROOT/nxaudio/include"
)

OBJS=()
compile_source() {
  group=$1
  source=$2
  base=$(basename "$source")
  object="$OBJDIR/${group}_${base%.*}.o"
  "$CC" -D_GNU_SOURCE -std=gnu11 "${COMMON_INCLUDES[@]}" \
    -idirafter /nxsr/usr/include \
    -idirafter /nxsr/usr/include/SDL2 \
    -idirafter /nxsr/usr/include/freetype2 \
    -O2 -fPIC -fno-omit-frame-pointer \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -c "$source" -o "$object"
  OBJS+=("$object")
}

# egl_shim.c fora: codigo morto da linhagem bully; um DT_NEEDED libEGL.so
# quebraria o carregamento onde o SONAME nao existe.
for source in src/*.c; do
  [ "$(basename "$source")" = "egl_shim.c" ] && continue
  compile_source ot "$source"
done
compile_source ot src/ot_setjmp.S

compile_source nxgl "$FRAMEWORK_ROOT/nxgl/adapters/nxgl_frame_proof_adapter.c"
for source in \
  "$FRAMEWORK_ROOT"/nxgl/adapters/nxgl_graphics_contract_adapter.c \
  "$FRAMEWORK_ROOT"/nxgl/adapters/nxgl_provider_discovery_adapter.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_graphics_contract.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_provider_recovery.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_arbiter.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_logic.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_sdl2.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_diagnostics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_metrics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_present.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_sdl_hint.c; do
  compile_source nxgl "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_gptk.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_gptk_loader.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_gptk_motion.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_exit_chord.c; do
  compile_source nxinput "$source"
done

compile_source nxaudio "$FRAMEWORK_ROOT/nxaudio/src/nxaudio_receipt.c"

# ---- stubs de link: gravam o SONAME certo sem importar a glibc do NextOS ----
# O aparelho fornece libSDL2-2.0.so.0 e libGLESv2.so.2. FreeType NAO entra
# nem como stub: text_render resolve por dlopen e degrada sem a lib.
UNDEFINED=$("$NM" --undefined-only "${OBJS[@]}" 2>/dev/null | awk '{print $NF}' | sort -u)

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

"$CC" -fPIE -pie -rdynamic -Wl,--strip-all -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -lGLESv2 -ldl -lm -lpthread

# O caminho aleatório de OBJDIR nunca pode entrar em .symtab/.strtab: além de
# não servir ao runtime, ele tornava o SHA-256 final diferente a cada build.
# --strip-all preserva a dynsym exportada por -rdynamic, auditada logo abaixo.

# Sem RPATH/RUNPATH: pacote universal nao embute caminho de busca; os modulos
# do jogo (libc++_shared/libgame) entram pelo so_util, nao pelo linker.

# ---- trava 0: integração V3 precisa existir no ELF final ----
DEFINED_SYMBOLS=$($NM -D --defined-only "$OUTPUT" | awk '{print $NF}')
for symbol in \
  otr_graphics_contract_start \
  otr_egl_import_table_build \
  nxgl_graphics_contract_validate \
  nxgl_graphics_contract_adapter_shader_probe \
  nxgl_graphics_contract_evidence_receipt \
  nxinput_gptk_load_at \
  nxinput_gptk_parse \
  nxinput_gptk_dispatcher_register \
  nxinput_exit_chord_update \
  nx_evdev_chord_poll \
  otr_exit_monitor_start \
  otr_exit_monitor_format_receipt \
  otr_gptk_note_terminal_receipt \
  otr_audio_recovery_run_callback_stalled \
  otr_audio_recovery_format \
  nxaudio_backend_recovery_run \
  nxaudio_receipt_format; do
  if ! grep -Fxq "$symbol" <<<"$DEFINED_SYMBOLS"; then
    echo "FALHA: símbolo V3 ausente do ELF: $symbol" >&2
    exit 1
  fi
done

# ---- trava 1: GLIBC <= 2.30 ----
MAX_GLIBC=$("$READELF" --version-info "$OUTPUT" |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
[ -n "$MAX_GLIBC" ] || { echo "nao foi possivel determinar a versao GLIBC" >&2; exit 1; }
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}; rest=${version_number#*.}; minor=${rest%%.*}
if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]; }; then
  echo "FALHA: $OUTPUT exige $MAX_GLIBC (limite GLIBC_2.30)" >&2
  exit 1
fi

# ---- trava 2: nenhum DT_NEEDED fora da linha de base universal ----
# libEGL.so ou libGLESv1_CM.so aqui = regressao (SONAME inexistente em CFW).
ALLOWED='^(libSDL2-2\.0\.so\.0|libGLESv2\.so\.2|libdl\.so\.2|libm\.so\.6|libpthread\.so\.0|libgcc_s\.so\.1|libc\.so\.6|ld-linux-aarch64\.so\.1)$'
BAD=$("$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}' |
  grep -Ev "$ALLOWED" || true)
if [ -n "$BAD" ]; then
  echo "FALHA: DT_NEEDED fora da linha de base universal:" >&2
  printf '  %s\n' $BAD >&2
  exit 1
fi

chown "${OT_HOST_UID:-0}:${OT_HOST_GID:-0}" "$OUTPUT" 2>/dev/null || true
printf 'offtheroad universal: %s, GLIBC max %s\n' "$OUTPUT" "$MAX_GLIBC"
"$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); printf "  NEEDED %s\n", $NF}'
