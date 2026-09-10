#!/bin/bash
# build_buster.sh -- build PUBLICO/UNIVERSAL do Blossom Tales.
#
# Alvo: AArch64, GLIBC no maximo 2.30 (regra canonica de publicacao da casa).
# Ambiente: Debian 10 (buster, glibc 2.28), gcc-8/g++-8 cross aarch64, imagem
# `blossomtales-builder:glibc230` (ver Dockerfile.builder).
# Saida: ./blossomtales-nextos
#
# O binario NextOS/Mali-450 anterior (`blossomtales`) exigia GLIBC_2.43 e por
# isso NAO serve ao pacote universal.
#
# A trilha decodifica no processo: FDK-AAC 2.0.3 (estatico) + minimp4. Nao ha
# `ffmpeg`/`ffprobe` externo. O C++ do FDK entra com -static-libstdc++ para
# nao acrescentar NEEDED nenhum ao ELF publico.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# O adapter de prova de imagem do nxgl entra COMPILADO no loader (a arquitetura
# da casa poe nxloader/nxcompat/nxgl/nxinput dentro do loader de cada ABI), por
# isso o container monta a raiz do repositorio e nao so a pasta do port.
REPO="$(cd "$HERE/../.." && pwd)"
PORTREL="ports/blossomtales"
IMAGE="${BT_BUILD_IMAGE:-blossomtales-builder:glibc230}"
OUT="${1:-blossomtales-nextos}"
FDK_VERSION=2.0.3
FDK_URL="https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v${FDK_VERSION}.tar.gz"
FDK_SHA256=e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78
FDK_SOURCE="$HERE/vendor/fdk-aac-${FDK_VERSION}"

# Worktrees novos não compartilham os inputs ignorados da árvore principal.
# Materialize a fonte oficial pinada antes do build e jamais compile um
# download cuja identidade não corresponda ao PINS.txt do port.
if [ ! -d "$FDK_SOURCE" ]; then
  FDK_FETCH_DIR=$(mktemp -d "$HERE/vendor/.fdk-fetch.XXXXXX")
  cleanup_fdk_fetch() {
    case "$FDK_FETCH_DIR" in
      "$HERE"/vendor/.fdk-fetch.*) rm -rf -- "$FDK_FETCH_DIR" ;;
      *) echo "refusing unsafe FDK cleanup path: $FDK_FETCH_DIR" >&2; return 1 ;;
    esac
  }
  trap cleanup_fdk_fetch EXIT
  echo "== obtendo FDK-AAC ${FDK_VERSION} pinado =="
  curl --fail --location --retry 3 --output "$FDK_FETCH_DIR/fdk-aac.tar.gz" "$FDK_URL"
  printf '%s  %s\n' "$FDK_SHA256" "$FDK_FETCH_DIR/fdk-aac.tar.gz" | sha256sum -c -
  tar -xzf "$FDK_FETCH_DIR/fdk-aac.tar.gz" -C "$FDK_FETCH_DIR"
  [ -d "$FDK_FETCH_DIR/fdk-aac-${FDK_VERSION}" ] || {
    echo "FDK-AAC: diretório esperado ausente no tar pinado" >&2
    exit 2
  }
  mv "$FDK_FETCH_DIR/fdk-aac-${FDK_VERSION}" "$FDK_SOURCE"
  cleanup_fdk_fetch
  trap - EXIT
fi

if [ ! -f "$HERE/vendor/build/libfdk-aac-dec.a" ]; then
  echo "== compilando FDK-AAC (decodificador) =="
  ${DOCKER:-docker} run --rm -v "$REPO:/repo" -w "/repo/$PORTREL" \
    --entrypoint /bin/sh "$IMAGE" -c "/repo/$PORTREL/vendor/build_fdk.sh"
fi

# O passo de compilacao mora num script proprio: assim as aspas de
# -DPORT_WINDOW_TITLE atravessam host -> docker -> sh sem se perder.
cat > "$HERE/.build_inner.sh" <<'INNER'
#!/bin/sh
set -e
cd "$(dirname "$0")"
OUT="$1"
FDK=vendor/fdk-aac-2.0.3

NXI=vendor/nxinput
SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.gen.c src/fbo_compat.c
src/bionic_shims.c src/pthread_bridge.c src/sdv_egl_bridge.c
src/present_fbo.c src/mono_trace.c src/fmod_shim.c src/aaudio_shim.c
src/astc_shim.c src/etc1.c src/media_player.c src/aac_decoder.c src/host_libs.c
src/util.c src/error.c src/bt_input.c src/input_gptk.c src/bt_sdl_shim.c
$NXI/src/nxinput_authority.c $NXI/src/nxinput_sdl.c $NXI/src/nxinput_godot.c $NXI/src/nxinput_exit_chord.c $NXI/src/nxinput_gptk.c
$NXI/src/nxinput_gptk_live.c $NXI/src/nxinput_gptk_loader.c $NXI/src/nxinput_gptk_motion.c
$NXI/src/nxinput_gptk_preinit.c $NXI/src/nxinput_livedb.c $NXI/src/nxinput_portmaster.c
$NXI/src/nxinput_sdl_seam.c $NXI/src/nxinput_sovereign.c
$NXI/src/nxinput_provider.c $NXI/src/nxinput_provider_linux.c $NXI/src/nxinput_sha256.c
$NXI/src/nxinput_translate.c $NXI/src/nxinput_decision.c
$NXI/src/nxinput_gptk4.c $NXI/src/nxinput_gptk4_bridge.c
$NXI/src/nxinput_prerouter.c $NXI/src/nxinput_axis_calib.c
vendor/nxcompat/src/nxcompat_settings.c vendor/nxcompat/src/nxcompat_video.c src/present_policy.c
$NXI/engine-glue/nxc6_glue.c $NXI/engine-glue/nxinput_padset.c
../../framework/nxgl/adapters/nxgl_frame_proof_adapter.c"
# BT_BENCH_BUILD=1 compila a variante de bancada (-DBT_BENCH_PROBES: diagnóstico
# por env). Nunca entra no ZIP; a build pública prova a ausência das strings.
BENCH_DEFS=""
[ "${BT_BENCH_BUILD:-0}" = 1 ] && BENCH_DEFS="-DBT_BENCH_PROBES=1"

# -D_GNU_SOURCE: a glibc 2.28 so expoe RTLD_DEFAULT sob _GNU_SOURCE.
# -include buster_compat.h: gettid() nao tem wrapper antes da glibc 2.30.
CFLAGS="-I src -I $NXI/include -I $NXI/engine-glue -I vendor/nxcompat/include -I /sdl2-include -I ../../framework/nxgl/adapters $BENCH_DEFS -I $FDK/libAACdec/include -I $FDK/libSYS/include
 -I $FDK/libFDK/include -I $FDK/libMpegTPDec/include
 -I $FDK/libPCMutils/include -I $FDK/libSBRdec/include
 -O2 -fPIC -fno-omit-frame-pointer -D_GNU_SOURCE -include buster_compat.h
 -Wno-unused-parameter -Wno-unused-function"

rm -rf /tmp/o && mkdir -p /tmp/o
for f in $SRCS; do
  aarch64-linux-gnu-gcc-8 $CFLAGS -DPORT_WINDOW_TITLE='"BlossomTales"' \
    -c "$f" -o "/tmp/o/$(basename "$f" .c).o"
done

# -rdynamic e' OBRIGATORIO: o so-loader resolve os imports das libs Android
# por dlsym(RTLD_DEFAULT, ...), e sem a .dynsym completa os shims bionic do
# proprio executavel (__system_property_get, __strlen_chk, __strchr_chk)
# ficam invisiveis -- o PLT salta para lixo e o Mono morre com SIGSEGV.
aarch64-linux-gnu-g++-8 -o "$OUT.dbg" /tmp/o/*.o \
  vendor/build/libfdk-aac-dec.a \
  -rdynamic -static-libstdc++ -static-libgcc \
  -Wl,--build-id=sha1 -Wl,-z,relro,-z,now \
  -ldl -lm -lpthread
aarch64-linux-gnu-objcopy --strip-unneeded "$OUT.dbg" "$OUT"
chmod 755 "$OUT" "$OUT.dbg"
case "${BT_OUTPUT_UID:-}:${BT_OUTPUT_GID:-}" in
  *[!0-9:]*|:|*:|:*)
    echo "invalid host output owner: ${BT_OUTPUT_UID:-}:${BT_OUTPUT_GID:-}" >&2
    exit 2
    ;;
esac
chown "$BT_OUTPUT_UID:$BT_OUTPUT_GID" "$OUT" "$OUT.dbg" \
  vendor/build/libfdk-aac-dec.a
INNER
chmod +x "$HERE/.build_inner.sh"

# Cabeçalhos SDL2 do sistema anfitrião (só declarações; a SDL de execução é a
# do firmware, resolvida por dlsym). A costura C6 (nxc6_glue.c) compila contra
# eles, como no FP2. Nunca entram no ELF nem no ZIP.
SDL2_INCLUDE_DIR="${SDL2_INCLUDE_DIR:-/usr/include/SDL2}"
[ -f "$SDL2_INCLUDE_DIR/SDL.h" ] || { echo "SDL2_INCLUDE_DIR sem SDL.h: $SDL2_INCLUDE_DIR" >&2; exit 2; }
${DOCKER:-docker} run --rm -e BT_BENCH_BUILD="${BT_BENCH_BUILD:-0}" \
  -e BT_OUTPUT_UID="$(id -u)" -e BT_OUTPUT_GID="$(id -g)" \
  -v "$REPO:/repo" -v "$SDL2_INCLUDE_DIR:/sdl2-include:ro" -w "/repo/$PORTREL" \
  --entrypoint /bin/sh "$IMAGE" -c "/repo/$PORTREL/.build_inner.sh '$OUT'"

echo "== $HERE/$OUT =="
aarch64-linux-gnu-readelf -d "$HERE/$OUT" | grep -E 'NEEDED|RPATH|RUNPATH' || true

# ---- Gates da build pública (nxinput-gptk vivo, nada de bancada no ELF) ----
if [ "${BT_BENCH_BUILD:-0}" != 1 ]; then
  STRINGS_OUT=$(strings -a "$HERE/$OUT")
  for must in nxinput-gptk-runtime/4 nxinput-gptk4-bridge/1 NXC6-PROVIDER NX-VIDEO/1 nxinput-gptk-event-evidence/1 NXC6-DOMAIN nxinput-padset/1 BT-FBO-COMPAT/1; do
    grep -qF -- "$must" <<<"$STRINGS_OUT" || { echo "GATE FAIL: literal ausente: $must" >&2; exit 4; }
  done
  for never in nxinput-gptk-runtime/2 nx_add_generic_gamepad_mappings "Generic Xbox Fallback" "Microsoft X-Box 360 pad" /dev/input/event \
               SB_TEST_KEY SB_INPUT_COMMANDS SB_INPUT_TRACE BT_INPUT_DIAG "[bt/key]" "[bt/sink]" /home/ watchdog heartbeat; do
    grep -qF -- "$never" <<<"$STRINGS_OUT" && { echo "GATE FAIL: literal proibido presente: $never" >&2; exit 4; }
  done
  # (sem `cmd | grep -q` sob pipefail: SIGPIPE no produtor derruba o gate com o recibo presente)
  DYNSYMS=$(LC_ALL=C readelf -W --dyn-syms "$HERE/$OUT")
  for sym in nxinput_gptk_live_seal nxinput_gptk_live_feed nxinput_gptk_live_feed_vector bt_sink_android_keyevent sb_engine_state_probe; do
    grep -Eq "FUNC +GLOBAL +DEFAULT +[0-9]+ +$sym\$" <<<"$DYNSYMS" || { echo "GATE FAIL: símbolo dinâmico ausente: $sym" >&2; exit 4; }
  done
  for sym in SDL_free SDL_GameControllerAddMapping SDL_JoystickGetGUIDFromString SDL_GameControllerMappingForGUID; do
    grep -Eq " $sym\$" <<<"$DYNSYMS" && { echo "GATE FAIL: trampolim SDL exportado: $sym" >&2; exit 4; }
  done
  grep -Eq " UND +SDL_" <<<"$DYNSYMS" && { echo "GATE FAIL: import SDL_* no ELF (o port não linka a SDL)" >&2; exit 4; }
  echo "GATES PUBLIC: PASS (gptk runtime/4, evidence/1, NXC6-DOMAIN, padset/1, FBO closure; sem bancada; sinks exportados; sem SDL no dynsym)"
fi
echo "max GLIBC: $(aarch64-linux-gnu-readelf -V "$HERE/$OUT" | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1)"
aarch64-linux-gnu-readelf -n "$HERE/$OUT" | grep -A1 'Build ID' | tail -1
ls -l "$HERE/$OUT"
sha256sum "$HERE/$OUT"
