#!/usr/bin/env bash
set -u

DRAFT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) ||
  exit 1
RUN_SH=${ASM2_TEST_RUN_SH:-}
OUTER_SH=${ASM2_TEST_OUTER_SH:-}
X5_ENV_SH=${ASM2_TEST_X5_ENV_SH:-}
if [ -z "$RUN_SH" ]; then
  if [ -f "$DRAFT_DIR/../package/sources/run.sh" ]; then
    RUN_SH="$DRAFT_DIR/../package/sources/run.sh"
  else
    RUN_SH="$DRAFT_DIR/run.sh"
  fi
fi
if [ -z "$OUTER_SH" ]; then
  if [ -f "$DRAFT_DIR/../package/sources/The Amazing Spider-Man 2.sh" ]; then
    OUTER_SH="$DRAFT_DIR/../package/sources/The Amazing Spider-Man 2.sh"
  else
    OUTER_SH="$DRAFT_DIR/The Amazing Spider-Man 2.sh"
  fi
fi
[ -n "$X5_ENV_SH" ] || X5_ENV_SH=$(dirname -- "$RUN_SH")/x5m-runtime-env.sh
[ -f "$X5_ENV_SH" ] ||
  {
    printf 'launcher-x5m-test: FAIL: X5M environment helper is missing: %s\n' \
      "$X5_ENV_SH" >&2
    exit 1
  }
FIXTURE=$(mktemp -d "${TMPDIR:-/tmp}/asm2-x5-launcher-test.XXXXXX") ||
  exit 1
TEST_PIDS=

fail() {
  printf 'launcher-x5m-test: FAIL: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  local pid
  for pid in $TEST_PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  rm -rf -- "$FIXTURE"
}
trap cleanup EXIT INT TERM

export ASM2_GAMEDIR="$FIXTURE/game"
mkdir -p \
  "$ASM2_GAMEDIR/bin" \
  "$ASM2_GAMEDIR/runtime/x5m/native" \
  "$FIXTURE/platform/etc" \
  "$FIXTURE/platform/proc/device-tree"

export ASM2_LAUNCHER_LIBRARY_ONLY=1
# shellcheck disable=SC1090
source "$RUN_SH"
unset ASM2_CFW_NAME ASM2_OS_NAME

BOX32_REAL=${ASM2_TEST_BOX32:-}
SDL2_REAL=${ASM2_TEST_SDL2_COMPAT:-}
GUEST_REAL=${ASM2_TEST_I386_LOADER:-}

[ -f "$BOX32_REAL" ] ||
  fail "set ASM2_TEST_BOX32 to the current AArch64 Box32 candidate"
[ -f "$SDL2_REAL" ] ||
  fail "set ASM2_TEST_SDL2_COMPAT to the AArch64 sdl2-compat candidate"
[ -f "$GUEST_REAL" ] ||
  fail "set ASM2_TEST_I386_LOADER to the current i386 loader candidate"

[ "$(sha256sum "$BOX32_REAL" | awk '{print $1}')" = \
  "$X5_BOX32_SHA256" ] || fail "Box32 frozen release hash is stale"
[ "$(sha256sum "$SDL2_REAL" | awk '{print $1}')" = \
  "$X5_SDL2_COMPAT_SHA256" ] || fail "SDL2 compatibility draft hash is stale"
[ "$(sha256sum "$GUEST_REAL" | awk '{print $1}')" = \
  "$X5_GUEST_BIN_SHA256" ] || fail "i386 frozen release hash is stale"

is_aarch64_executable "$BOX32_REAL" ||
  fail "AArch64 Box32 was rejected by the readelf-free parser"
is_aarch64_shared_object "$SDL2_REAL" ||
  fail "AArch64 SDL2 compatibility library was rejected"
is_i386_executable "$GUEST_REAL" ||
  fail "i386 ASM2 loader was rejected by the readelf-free parser"
is_i386_executable /bin/sh &&
  fail "host /bin/sh was accepted as an i386 ASM2 loader"
is_aarch64_executable "$GUEST_REAL" &&
  fail "i386 ASM2 loader was accepted as AArch64"

box_interpreter=$(elf_interpreter "$BOX32_REAL") ||
  fail "AArch64 Box32 interpreter was not parsed"
case "$box_interpreter" in
  /*) ;;
  *) fail "AArch64 Box32 interpreter was not absolute" ;;
esac

printf 'NAME="NextOS"\nID=nextos\n' > "$FIXTURE/platform/etc/os-release"
printf 'amlogic, s7d\0' > "$FIXTURE/platform/proc/device-tree/compatible"
is_tested_x5m_nextos "$FIXTURE/platform" aarch64 ||
  fail "exact NextOS/S7D/AArch64 fixture was not selected"
is_tested_x5m_nextos "$FIXTURE/platform" armv7l &&
  fail "ARMv7 host entered the Box32 path"

select_platform "$FIXTURE/platform" aarch64 >/dev/null
[ "$PLATFORM_KIND" = x5m-box32 ] ||
  fail "X5M fixture selected $PLATFORM_KIND"
[ "$BIN" = "$X5_GUEST_BIN" ] ||
  fail "X5M fixture selected the wrong guest loader"
[ "$PLATFORM_HELPER_TARGET" = "$X5_BOX32_BIN" ] ||
  fail "PortMaster helper target is not the AArch64 host"

printf 'amlogic, g12b\0' > "$FIXTURE/platform/proc/device-tree/compatible"
select_platform "$FIXTURE/platform" aarch64 >/dev/null
[ "$PLATFORM_KIND" = armhf ] && [ "$BIN" = "$ARM_NEXTOS_BIN" ] ||
  fail "non-S7D NextOS behavior changed from the ARM path"

printf 'amlogic, s7d\0' > "$FIXTURE/platform/proc/device-tree/compatible"
printf 'NAME="ArkOS"\nID=arkos\n' > "$FIXTURE/platform/etc/os-release"
select_platform "$FIXTURE/platform" aarch64 >/dev/null
[ "$PLATFORM_KIND" = armhf ] && [ "$BIN" = "$ARM_PORTMASTER_BIN" ] ||
  fail "S7D without NextOS entered the private Box32 path"

unset \
  BOX64_NOPERSONA32BITS \
  BOX64_DYNAREC \
  BOX64_DYNAREC_BIGBLOCK \
  BOX64_DYNAREC_SAFEFLAGS \
  SDL_VIDEODRIVER \
  SDL_VIDEO_DRIVER \
  SDL_KMSDRM_DEVICE_INDEX \
  SDL_KMSDRM_REQUIRE_DRM_MASTER
launcher_ld_library_path=${LD_LIBRARY_PATH:-}
configure_x5m_runtime
[ "$BOX64_NOPERSONA32BITS" = 1 ] ||
  fail "Box32 32-bit persona guard is missing"
[ "$BOX64_DYNAREC" = 1 ] &&
  [ "$BOX64_DYNAREC_BIGBLOCK" = 0 ] &&
  [ "$BOX64_DYNAREC_SAFEFLAGS" = 2 ] ||
  fail "validated X5M dynarec safety profile is incomplete"
[ "$SDL_VIDEODRIVER" = kmsdrm ] &&
  [ "$SDL_VIDEO_DRIVER" = kmsdrm ] &&
  [ "$SDL_KMSDRM_DEVICE_INDEX" = 0 ] &&
  [ "$SDL_KMSDRM_REQUIRE_DRM_MASTER" = 1 ] ||
  fail "tested X5M KMSDRM environment is incomplete"
[ "${LD_LIBRARY_PATH:-}" = "$launcher_ld_library_path" ] ||
  fail "X5M game SDL compatibility path leaked into the extractor environment"
grep -Fq \
  'LD_LIBRARY_PATH="$native_lib_dir:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \' \
  "$X5_ENV_SH" ||
  fail "X5M native SDL compatibility directory is not scoped to game launch"
grep -Fq \
  'LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$control_folder/libs:$control_folder/libs.aarch64" \' \
  "$X5_ENV_SH" ||
  fail "X5M extractor does not prefer the proven firmware SDL2 stack"

kms_assignments=$(grep -Ec \
  '^[[:space:]]*export (SDL_VIDEODRIVER|SDL_VIDEO_DRIVER|SDL_KMSDRM_DEVICE_INDEX|SDL_KMSDRM_REQUIRE_DRM_MASTER)=' \
  "$RUN_SH")
[ "$kms_assignments" -eq 4 ] ||
  fail "unexpected count of KMSDRM assignments: $kms_assignments"
awk '
  /^configure_x5m_runtime[(][)] \{/ { inside=1 }
  inside { print }
  inside && /^}/ { exit }
' "$RUN_SH" > "$FIXTURE/x5-function.txt"
grep -Eq 'export SDL_VIDEODRIVER=kmsdrm' "$FIXTURE/x5-function.txt" ||
  fail "KMSDRM assignment escaped its X5M-only function"
all_kms_lines=$(grep -En \
  '^[[:space:]]*export (SDL_VIDEODRIVER|SDL_VIDEO_DRIVER|SDL_KMSDRM_DEVICE_INDEX|SDL_KMSDRM_REQUIRE_DRM_MASTER)=' \
  "$RUN_SH")
x5_kms_lines=$(grep -En \
  '^[[:space:]]*export (SDL_VIDEODRIVER|SDL_VIDEO_DRIVER|SDL_KMSDRM_DEVICE_INDEX|SDL_KMSDRM_REQUIRE_DRM_MASTER)=' \
  "$FIXTURE/x5-function.txt")
[ "$(printf '%s\n' "$all_kms_lines" | wc -l)" -eq \
  "$(printf '%s\n' "$x5_kms_lines" | wc -l)" ] ||
  fail "a KMSDRM assignment exists outside configure_x5m_runtime"

dynarec_assignments=$(grep -Ec \
  '^[[:space:]]*export BOX64_DYNAREC(=|_(BIGBLOCK|SAFEFLAGS)=)' \
  "$RUN_SH")
[ "$dynarec_assignments" -eq 3 ] ||
  fail "unexpected count of validated dynarec safety assignments"
for dynarec_assignment in \
  'export BOX64_DYNAREC=1' \
  'export BOX64_DYNAREC_BIGBLOCK=0' \
  'export BOX64_DYNAREC_SAFEFLAGS=2'; do
  grep -Fq "$dynarec_assignment" "$FIXTURE/x5-function.txt" ||
    fail "dynarec safety assignment escaped configure_x5m_runtime"
done
if grep -En \
  '^[[:space:]]*(export[[:space:]]+)?BOX64_DYNAREC_EAGER=' \
  "$RUN_SH" "$OUTER_SH"; then
  fail "launcher enables experimental eager mode"
fi

# Process identity: executable path with the literal "(deleted)" suffix.
cp /bin/sleep "$X5_BOX32_BIN" || fail "could not build process fixture"
(
  cd "$ASM2_GAMEDIR" || exit 1
  exec "$X5_BOX32_BIN" 30
) &
deleted_pid=$!
TEST_PIDS="$TEST_PIDS $deleted_pid"
rm -f "$X5_BOX32_BIN"
sleep 0.1
deleted_exe=$(readlink "/proc/$deleted_pid/exe" 2>/dev/null || true)
case "$deleted_exe" in
  "$X5_BOX32_BIN (deleted)") ;;
  *) fail "kernel did not expose the deleted ELF identity: $deleted_exe" ;;
esac
matching_game_pids | grep -Fxq "$deleted_pid" ||
  fail "deleted Box32 process was not enumerated"

# Process identity by command line while executable and comm are generic.
(
  cd "$ASM2_GAMEDIR" || exit 1
  exec -a "$X5_GUEST_BIN" /bin/sleep 30
) &
cmd_pid=$!
TEST_PIDS="$TEST_PIDS $cmd_pid"
sleep 0.1
matching_game_pids | grep -Fxq "$cmd_pid" ||
  fail "i386 guest command-line identity was not enumerated"

# Process identity by the 15-byte comm value while exe resolves elsewhere.
ln -s /bin/sleep "$X5_GUEST_BIN" ||
  fail "could not build comm process fixture"
(
  cd "$ASM2_GAMEDIR" || exit 1
  exec "$X5_GUEST_BIN" 30
) &
comm_pid=$!
TEST_PIDS="$TEST_PIDS $comm_pid"
sleep 0.1
comm_value=$(cat "/proc/$comm_pid/comm" 2>/dev/null || true)
case "$comm_value" in
  asm2_127_x86_*) ;;
  *) fail "comm fixture has an unexpected value: $comm_value" ;;
esac
matching_game_pids | grep -Fxq "$comm_pid" ||
  fail "i386 guest comm identity was not enumerated"

kill "$deleted_pid" "$cmd_pid" "$comm_pid" 2>/dev/null || true
wait "$deleted_pid" "$cmd_pid" "$comm_pid" 2>/dev/null || true
TEST_PIDS=

restart_mode=
restart_calls=0
restart_stops=0
restart_sleeps=0
run_game_once() {
  restart_calls=$((restart_calls + 1))
  case "$restart_mode:$restart_calls" in
    once:1) return 75 ;;
    once:2) return 0 ;;
    loop:*) return 75 ;;
    ordinary:*) return 42 ;;
  esac
}
stop_existing_game() {
  restart_stops=$((restart_stops + 1))
}
sleep() {
  [ "$1" = 1 ] || fail "restart delay changed from one second"
  restart_sleeps=$((restart_sleeps + 1))
}

restart_mode=once
run_game_foreground
[ "$?" -eq 0 ] && [ "$restart_calls" -eq 2 ] &&
  [ "$restart_stops" -eq 1 ] && [ "$restart_sleeps" -eq 1 ] ||
  fail "rc75 -> rc0 did not restart exactly once"

restart_mode=loop
restart_calls=0
restart_stops=0
restart_sleeps=0
run_game_foreground
[ "$?" -eq 75 ] && [ "$restart_calls" -eq 2 ] &&
  [ "$restart_stops" -eq 1 ] && [ "$restart_sleeps" -eq 1 ] ||
  fail "repeated rc75 was not bounded to one restart"

restart_mode=ordinary
restart_calls=0
restart_stops=0
restart_sleeps=0
run_game_foreground
[ "$?" -eq 42 ] && [ "$restart_calls" -eq 1 ] &&
  [ "$restart_stops" -eq 0 ] && [ "$restart_sleeps" -eq 0 ] ||
  fail "ordinary status was not preserved"

extract_line=$(grep -n 'prepare_owner_data || return' "$RUN_SH" |
  cut -d: -f1)
x86_validate_line=$(grep -n 'validate_x5m_owner_data' "$RUN_SH" |
  tail -1 | cut -d: -f1)
[ -n "$extract_line" ] && [ -n "$x86_validate_line" ] &&
  [ "$extract_line" -lt "$x86_validate_line" ] ||
  fail "i386 owner library is touched before extraction"

if grep -En \
  '(^|[[:space:]])(setsid|nohup|systemctl)([[:space:]]|$)' \
  "$RUN_SH" "$OUTER_SH"; then
  fail "public launcher contains a forbidden lifecycle command"
fi

finish_calls=$(grep -Ec \
  '^[[:space:]]*command -v pm_finish .*&& pm_finish[[:space:]]*$' \
  "$OUTER_SH")
[ "$finish_calls" -eq 1 ] ||
  fail "outer launcher must own exactly one pm_finish call"
grep -Fq 'pm_platform_helper "$PLATFORM_HELPER_TARGET"' "$RUN_SH" ||
  fail "non-X5M routes no longer delegate frontend setup to PortMaster"
grep -Fq 'using Horizon-style X5M foreground lifecycle' "$RUN_SH" ||
  fail "inner launcher does not preserve the proven X5M foreground lifecycle"
grep -Fq \
  'asm2_x5m_run_game "$GAMEDIR" "$X5_NATIVE_LIB_DIR" "$X5_BOX32_BIN"' \
  "$RUN_SH" ||
  fail "Box32 helper invocation contract changed"
grep -Fq '"$box32_bin" ./asm2_127_x86_box32 "$game_dir"' "$X5_ENV_SH" ||
  fail "Box32 invocation contract changed"

bash -n "$RUN_SH" || fail "run.sh failed bash -n"
bash -n "$OUTER_SH" || fail "outer launcher failed bash -n"
bash -n "$X5_ENV_SH" || fail "X5M environment helper failed bash -n"

printf 'launcher-x5m-test: PASS\n'
