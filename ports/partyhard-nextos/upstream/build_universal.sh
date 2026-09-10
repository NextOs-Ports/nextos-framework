#!/usr/bin/env bash
# Reproducible AArch64 build for the public/multi-device Party Hard GO runtime.
#
# The already-approved private NextOS binary (build.sh, current NextOS glibc)
# remains untouched.  Objects are compiled with the pinned modern cross
# compiler because this loader relies on atomics in its Unity, audio and input
# bridges.  Debian Buster headers and the final Buster link keep the Linux ABI
# at GLIBC <= 2.30.  The current NextOS sysroot supplies API headers only; none
# of its libc or SDL objects are linked.  Same route as the approved FP2 1.1.3
# and Dead Trigger 1.0.8 universal builds.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# ST_BENCH_BUILD=1 compila a VARIANTE DE BANCADA (-DST_BENCH_PROBES: pad
# virtual simbólico, sondas de input) em build/bench/.  Nunca entra no ZIP: a
# build pública não compila esses caminhos e os gates abaixo provam que as
# strings não existem no ELF público.
ST_BENCH_BUILD=${ST_BENCH_BUILD:-0}
if [[ $ST_BENCH_BUILD == 1 ]]; then
  OUTPUT_REL=build/bench/partyhard-nextos
  BENCH_CFLAGS=(-DST_BENCH_PROBES=1)
else
  OUTPUT_REL=build/universal/partyhard-nextos
  BENCH_CFLAGS=()
fi
if [[ -n ${NX_PUBLIC_FINAL_OUTPUT_DIR:-} && $ST_BENCH_BUILD != 1 ]]; then
  OUTPUT_PATH=$NX_PUBLIC_FINAL_OUTPUT_DIR/partyhard-nextos
else
  OUTPUT_PATH=$PORT_DIR/$OUTPUT_REL
fi
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
COMPILER_VERSION=16.1.0
COMPILER_SHA256=2fcae05bfafdfc6c0c3453431ce4538fdb8b481169c4670ae48be48a28e4fbe9
CC1_SHA256=1e947f6ecee4219b010307e7dd0cd00dae78b9c8533c3e3d852a1a6c1d209a7b
ASSEMBLER_SHA256=d798e501ac1a4a674106e132865ba18236bb7e36304d18a1b886595b839f9f44
GCC_INCLUDE_SHA256=cf5b4b60eb06a121accd66559ee2b99755c41390ce915c221123e86745bd00a5
GCC_FIXED_SHA256=b1486527d346fee9305644202c3ca3231f0eb8824bfaa39f66922c3bb95c095d
NEXTOS_API_HEADERS_SHA256=77b3eddae1e7cb52ac5b4d5b30dc5aab9c1be36ae1cf82294bcd7ecd6bf3f1ca
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786233600}

fail() {
  printf 'partyhard universal build error: %s\n' "$*" >&2
  exit 1
}

for tool in docker aarch64-linux-gnu-gcc aarch64-linux-gnu-nm \
            aarch64-linux-gnu-readelf aarch64-linux-gnu-strip \
            file sha256sum strings; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing build tool: $tool"
done

CC=$(command -v aarch64-linux-gnu-gcc)
CC1=$($CC -print-prog-name=cc1)
ASSEMBLER=$($CC -print-prog-name=as)
[[ $($CC -dumpmachine) == aarch64-linux-gnu &&
   $($CC -dumpfullversion -dumpversion) == "$COMPILER_VERSION" ]] ||
  fail "pinned AArch64 GCC $COMPILER_VERSION is required"
printf '%s  %s\n' "$COMPILER_SHA256" "$CC" | sha256sum -c - >/dev/null
printf '%s  %s\n' "$CC1_SHA256" "$CC1" | sha256sum -c - >/dev/null
printf '%s  %s\n' "$ASSEMBLER_SHA256" "$ASSEMBLER" | sha256sum -c - >/dev/null

ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" \
  --format '{{.Id}}' 2>/dev/null) ||
  fail "offline builder image is missing: $BUILDER_IMAGE"
[[ $ACTUAL_IMAGE_ID == "$BUILDER_IMAGE_ID" ]] ||
  fail "builder image changed: $ACTUAL_IMAGE_ID"

NEXTOS_ROOT=${NEXTOS_ROOT:-}
NEXTOS_SYSROOT=${NEXTOS_SYSROOT:-}
if [[ -z $NEXTOS_SYSROOT ]]; then
  [[ -n $NEXTOS_ROOT ]] ||
    fail "set NEXTOS_SYSROOT directly or NEXTOS_ROOT to a NextOS source tree"
  while IFS= read -r candidate; do
    sysroot=$candidate/aarch64-libreelec-linux-gnu/sysroot
    [[ -d $sysroot/usr/include/SDL2 ]] || continue
    [[ -d $sysroot/usr/include/EGL ]] || continue
    [[ -d $sysroot/usr/include/GLES2 ]] || continue
    NEXTOS_SYSROOT=$sysroot
  done < <(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V
  )
fi
[[ -d $NEXTOS_SYSROOT/usr/include/SDL2 &&
   -d $NEXTOS_SYSROOT/usr/include/EGL &&
   -d $NEXTOS_SYSROOT/usr/include/GLES2 &&
   -d $NEXTOS_SYSROOT/usr/include/KHR ]] ||
  fail "NextOS API-header sysroot is incomplete: $NEXTOS_SYSROOT"
actual_headers_sha256=$(
  cd "$NEXTOS_SYSROOT/usr/include"
  find SDL2 EGL GLES2 KHR -type f -print0 | sort -z |
    xargs -0 sha256sum | sha256sum | awk '{print $1}'
)
[[ $actual_headers_sha256 == "$NEXTOS_API_HEADERS_SHA256" ]] ||
  fail "NextOS API headers changed: $actual_headers_sha256"

BUILD_TMP_ROOT=${ST_BUILD_TMP_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$BUILD_TMP_ROOT"
WORK_DIR=$(mktemp -d "$BUILD_TMP_ROOT/partyhard-universal-build.XXXXXX")
cleanup() {
  find "$WORK_DIR" -depth -delete 2>/dev/null || true
}
trap cleanup EXIT INT TERM
mkdir -p "$WORK_DIR/obj" "$WORK_DIR/out"
install -m 0644 "$PORT_DIR/package/partyhard-public-symbols.list" \
  "$WORK_DIR/partyhard-public-symbols.list"

# Copy only Buster's target libc headers out of the pinned, offline image.
docker run --rm --network none \
  --user "$(id -u):$(id -g)" \
  -v "$WORK_DIR":/work \
  "$BUILDER_IMAGE_ID" \
  sh -c 'cp -a /usr/aarch64-linux-gnu/include /work/buster-include'

GCC_INCLUDE=$($CC -print-file-name=include)
GCC_FIXED=$(dirname "$GCC_INCLUDE")/include-fixed
[[ -d $GCC_INCLUDE && -d $GCC_FIXED ]] ||
  fail "pinned GCC internal header trees are incomplete"
actual_gcc_include_sha256=$(
  cd "$GCC_INCLUDE"
  find . -type f -print0 | sort -z |
    xargs -0 sha256sum | sha256sum | awk '{print $1}'
)
[[ $actual_gcc_include_sha256 == "$GCC_INCLUDE_SHA256" ]] ||
  fail "GCC include tree changed: $actual_gcc_include_sha256"
actual_gcc_fixed_sha256=$(
  cd "$GCC_FIXED"
  find . -type f -print0 | sort -z |
    xargs -0 sha256sum | sha256sum | awk '{print $1}'
)
[[ $actual_gcc_fixed_sha256 == "$GCC_FIXED_SHA256" ]] ||
  fail "GCC include-fixed tree changed: $actual_gcc_fixed_sha256"
COMMON_INCLUDES=(
  -I "$PORT_DIR/src"
  -I "$PORT_DIR/vendor/nxinput/include"
  -I "$PORT_DIR/vendor/nxinput/engine-glue"
  -I "$PORT_DIR/vendor/nxinput/src"
  -I "$PORT_DIR/vendor/nxcompat/include"
  -nostdinc
  -isystem "$GCC_INCLUDE"
)
COMMON_INCLUDES+=( -isystem "$GCC_FIXED" )
COMMON_INCLUDES+=(
  -isystem "$WORK_DIR/buster-include"
  -idirafter "$NEXTOS_SYSROOT/usr/include"
  -idirafter "$NEXTOS_SYSROOT/usr/include/SDL2"
)

# The port's own sources plus the vendored nxinput seam/GPTK runtime, each
# pinned byte-for-byte against its framework authority (PINS.json).
python3 - "$PORT_DIR/vendor/nxinput" "$PORT_DIR/vendor/nxcompat" <<'PY'
import hashlib, json, os, sys
for root in sys.argv[1:]:
    pins = json.load(open(os.path.join(root, "PINS.json")))
    for rel, digest in pins["files"].items():
        if hashlib.sha256(open(os.path.join(root, rel), "rb").read()).hexdigest() != digest:
            raise SystemExit("vendored framework file drifted from PINS.json: %s" % rel)
PY
mapfile -t SOURCES < <(
  { find "$PORT_DIR/src" -maxdepth 1 -type f -name '*.c' -print
    find "$PORT_DIR/vendor/nxinput/src" -maxdepth 1 -type f -name '*.c' -print
    find "$PORT_DIR/vendor/nxinput/engine-glue" -maxdepth 1 -type f -name '*.c' -print
    find "$PORT_DIR/vendor/nxcompat/src" -maxdepth 1 -type f -name '*.c' -print
  } | sort
)
[[ ${#SOURCES[@]} -gt 0 ]] || fail "no C sources found"
grep -Fq 'if (setenv("GC_DISABLE_INCREMENTAL", "1", 1) != 0)' \
  "$PORT_DIR/src/contract.c" ||
  fail "Unity incremental GC disablement is not authoritative"
grep -Fq 'if (setenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0", 1) != 0)' \
  "$PORT_DIR/src/contract.c" ||
  fail "positional SDL controller labels are not authoritative"
python3 "$PORT_DIR/tests/test_input_release_contract.py" ||
  fail "input release contract regressed"
python3 "$PORT_DIR/tests/test_health_receipt.py" ||
  fail "run-bound health receipt contract regressed"
python3 "$PORT_DIR/tests/test_key_event_payload.py" ||
  fail "per-event KeyEvent payload contract regressed"

OBJS=()
index=0
for source in "${SOURCES[@]}"; do
  index=$((index + 1))
  printf -v object '%s/obj/%03d.o' "$WORK_DIR" "$index"
  "$CC" -std=gnu11 "${COMMON_INCLUDES[@]}" \
    -O2 -fPIE -mno-outline-atomics \
    -fno-strict-aliasing -fno-omit-frame-pointer \
    -fno-stack-protector -fno-ident \
    -DST_RELEASE_BUILD=1 "${BENCH_CFLAGS[@]}" -ffunction-sections -fdata-sections \
    -ffile-prefix-map="$PORT_DIR"=. -fdebug-prefix-map="$PORT_DIR"=. \
    -Wdate-time -Werror=date-time \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# GCC 8 performs only the final ABI link.  A SONAME-only SDL stub records the
# firmware dependency without vendoring or linking a private SDL implementation.
docker run --rm --network none \
  --user "$(id -u):$(id -g)" \
  -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
  -v "$WORK_DIR":/work \
  "$BUILDER_IMAGE_ID" \
  bash -c '
set -euo pipefail
objects=(/work/obj/*.o)
undefined=$(
  aarch64-linux-gnu-nm --undefined-only "${objects[@]}" 2>/dev/null |
    awk "{print \$NF}" | sort -u
)
: > /work/sdl.c
while IFS= read -r symbol; do
  [[ $symbol == SDL_* ]] || continue
  printf "void %s(void) {}\n" "$symbol" >> /work/sdl.c
done <<< "$undefined"
aarch64-linux-gnu-gcc -shared -fPIC -nostdlib \
  -Wl,-soname,libSDL2-2.0.so.0 /work/sdl.c -o /work/libSDL2.so
aarch64-linux-gnu-gcc -fPIE -pie -o /work/out/partyhard-nextos \
  "${objects[@]}" \
  -L/work -Wl,--no-as-needed -lSDL2 -Wl,--as-needed \
  -ldl -lm -lpthread -lz -lgcc_s -Wl,--gc-sections \
  -Wl,--dynamic-list=/work/partyhard-public-symbols.list \
  -Wl,--build-id=sha1 -Wl,-z,relro,-z,now,-z,noexecstack
aarch64-linux-gnu-strip --strip-all /work/out/partyhard-nextos
'

mkdir -p "$(dirname "$OUTPUT_PATH")"
install -m 0755 "$WORK_DIR/out/partyhard-nextos" "$OUTPUT_PATH"

READELF=aarch64-linux-gnu-readelf
MAX_GLIBC=$(
  "$READELF" --version-info "$OUTPUT_PATH" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1
)
[[ -n $MAX_GLIBC ]] || fail "unable to determine GLIBC requirement"
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
(( major < 2 || (major == 2 && minor <= 30) )) ||
  fail "$OUTPUT_PATH requires $MAX_GLIBC; limit is GLIBC_2.30"

MACHINE=$(
  "$READELF" -h "$OUTPUT_PATH" |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p'
)
[[ $MACHINE == AArch64 ]] || fail "unexpected architecture: $MACHINE"
INTERPRETER=$(
  "$READELF" -lW "$OUTPUT_PATH" |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p'
)
[[ $INTERPRETER == /lib/ld-linux-aarch64.so.1 ]] ||
  fail "unexpected interpreter: $INTERPRETER"
if "$READELF" -lW "$OUTPUT_PATH" |
    awk '$1 == "LOAD" && $0 ~ /RWE/ {bad=1} END {exit !bad}'; then
  fail "runtime contains an RWX PT_LOAD"
fi
if "$READELF" -dW "$OUTPUT_PATH" | grep -Eq '\((RPATH|RUNPATH)\)'; then
  fail "runtime contains forbidden RPATH/RUNPATH"
fi

NEEDED=$(
  "$READELF" -dW "$OUTPUT_PATH" |
    awk -F'[][]' '/NEEDED/ {print $2}' | sort
)
ALLOWED=$(
  printf '%s\n' libSDL2-2.0.so.0 libc.so.6 libdl.so.2 libgcc_s.so.1 \
    libm.so.6 libpthread.so.0 libz.so.1 | sort
)
[[ $NEEDED == "$ALLOWED" ]] || {
  printf 'unexpected DT_NEEDED set:\n%s\n' "$NEEDED" >&2
  exit 1
}
file "$OUTPUT_PATH" | grep -q ', stripped' || fail "runtime is not stripped"
if "$READELF" -S "$OUTPUT_PATH" | grep -Eq '\.(symtab|debug)'; then
  fail "runtime retains symbol/debug sections"
fi
for symbol in nxgl_frame_proof_before_present nxgl_frame_proof_publish \
              nxgl_graphics_contract_validate \
              nxgl_graphics_contract_adapter_shader_probe \
              nxgl_graphics_contract_evidence_receipt; do
  "$READELF" --dyn-syms -W "$OUTPUT_PATH" |
    awk -v wanted="$symbol" '$8 == wanted {found=1} END {exit !found}' ||
    fail "runtime does not export required video-proof symbol: $symbol"
done
for literal in NXBOOTSTRAP_VIDEO_FILE org.nextos.nxruntime.video-proof \
               VIDEO-PROOF: GRAPHICS-EVIDENCE: nx-graphics-evidence \
               NXBOOTSTRAP_HEALTH_FILE NXBOOTSTRAP_HEALTH_RUN_ID \
               'Party Hard GO public release requires a connected controller'; do
  grep -Fq "$literal" < <(strings "$OUTPUT_PATH") ||
    fail "runtime lacks required video-proof literal: $literal"
done
# V5 controls/video identity (nxrelease closes the same contract).
for literal in nxinput-gptk-runtime/4 nxinput-gptk4-bridge/1 \
               nxinput-gptk-event-evidence/1 NXC6-PROVIDER NXC6-DOMAIN \
               NX-VIDEO/1 /usr/lib/gamecontrollerdb.txt \
               SDL_JoystickPathForIndex policy=neutral-only; do
  [[ $(strings "$OUTPUT_PATH" | grep -Fc "$literal") -gt 0 ]] ||
    fail "runtime lacks required live-controls identity: $literal"
done
# Autoridades falsas de mapping (C3): joystick cru posicional, varredura
# propria de evdev e fallback generico nunca voltam.  A identidade
# "Microsoft X-Box 360 pad" NAO e' autoridade de mapping: e' o nome pelo qual
# o InControl do jogo escolhe o perfil de botoes (comportamento aprovado).
for literal in nxinput-gptk-runtime/2 nx_add_generic_gamepad_mappings \
               'Generic Xbox Fallback' /dev/input/event TRIGGER_HAPPY; do
  if [[ $(strings "$OUTPUT_PATH" | grep -Fc "$literal") -gt 0 ]]; then
    fail "runtime retains a forbidden controls authority: $literal"
  fi
done
for symbol in nxinput_gptk_live_seal nxinput_gptk_live_feed \
              nxinput_gptk_live_feed_vector st_sink_android_keyevent \
              st_sink_android_motion st_sink_cursor st_sink_cursor_click; do
  "$READELF" --dyn-syms -W "$OUTPUT_PATH" |
    awk -v wanted="$symbol" '$8 == wanted {found=1} END {exit !found}' ||
    fail "runtime does not export required live-controls symbol: $symbol"
done
if [[ $ST_BENCH_BUILD != 1 ]]; then
  for literal in /tmp/st-vpad '[st/vpad]' ST_VPAD ST_INPUT_DIAG \
                 ST_INPUT_ENGINE_PROBE ST_INPUT_STALE_BUTTON \
                 ST_INPUT_STALE_AXIS /tmp/bcgp /tmp/bcshot; do
    if [[ $(strings "$OUTPUT_PATH" | grep -Fc -- "$literal") -gt 0 ]]; then
      fail "bench-only injection path present in the public ELF: $literal"
    fi
  done
fi
if grep -Eq 'netflix_selftest|/home/|192\.168\.' < <(strings "$OUTPUT_PATH"); then
  fail "runtime contains development instrumentation or personal data"
fi

printf 'PARTYHARD UNIVERSAL BUILD OK -> %s\n' "$OUTPUT_PATH"
printf 'maximum glibc: %s (limit GLIBC_2.30)\n' "$MAX_GLIBC"
printf 'build id: %s\n' \
  "$("$READELF" -n "$OUTPUT_PATH" | sed -n 's/.*Build ID: //p')"
sha256sum "$OUTPUT_PATH"
