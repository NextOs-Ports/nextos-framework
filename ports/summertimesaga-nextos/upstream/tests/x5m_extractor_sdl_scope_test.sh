#!/usr/bin/env bash
# Contract test for the proven native NXExtract path on NextOS S905X5M.
set -euo pipefail

OUTER_SH=${1:-}
ENV_SH=${2:-}
UPDATE_SH=${3:-}

fail() {
  printf 'Summertime X5M extractor scope test: FAIL: %s\n' "$*" >&2
  exit 1
}

for script in "$OUTER_SH" "$ENV_SH" "$UPDATE_SH"; do
  [ -f "$script" ] || fail "usage: $0 OUTER_SH ENV_SH UPDATE_SH"
  bash -n "$script"
done

fixture=$(mktemp -d "${TMPDIR:-/tmp}/summertime-x5m-scope.XXXXXX")
cleanup() {
  find "$fixture" -depth -delete 2>/dev/null || true
}
trap cleanup EXIT INT TERM

root=$fixture/root
game=$fixture/game
control=$fixture/control
mkdir -p "$root/etc" "$root/proc/device-tree" \
  "$game" "$control/libs" "$control/libs.aarch64"
printf '%s\n' 'ID=nextos' > "$root/etc/os-release"
printf 'amlogic, s7d\0' > "$root/proc/device-tree/compatible"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "ld=%s\n" "${LD_LIBRARY_PATH:-<unset>}"' \
  'printf "video=%s\n" "${SDL_VIDEODRIVER:-<unset>}"' \
  'printf "video_alias=%s\n" "${SDL_VIDEO_DRIVER:-<unset>}"' \
  'printf "game=%s\n" "${NXEXTRACT_GAME_DIR:-<unset>}"' \
  'printf "args=%s\n" "$*"' \
  > "$game/run-extractor.sh"
chmod +x "$game/run-extractor.sh"

# shellcheck source=/dev/null
source "$ENV_SH"
declare -F summertime_run_extractor >/dev/null 2>&1 ||
  fail "runtime helper does not export summertime_run_extractor"
summertime_is_tested_x5m_nextos "$root" aarch64 ||
  fail "the exact NextOS S7D fixture was not detected"
summertime_is_tested_x5m_nextos "$root" x86_64 &&
  fail "a non-AArch64 machine was accepted"

output=$(
  LD_LIBRARY_PATH=/game-only/leak \
  SDL_VIDEODRIVER=wrong-driver \
  SDL_VIDEO_DRIVER=wrong-alias \
    summertime_run_extractor \
      "$game" "$control" "$root" aarch64 --force-source --input future.bin
)
expected_ld="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$control/libs:$control/libs.aarch64"
grep -Fxq "ld=$expected_ld" <<< "$output" ||
  fail "the X5M extractor did not receive the clean firmware library scope"
grep -Fxq 'video=<unset>' <<< "$output" ||
  fail "SDL_VIDEODRIVER leaked into the X5M extractor"
grep -Fxq 'video_alias=<unset>' <<< "$output" ||
  fail "SDL_VIDEO_DRIVER leaked into the X5M extractor"
grep -Fxq "game=$game" <<< "$output" ||
  fail "NXEXTRACT_GAME_DIR was not preserved"
grep -Fxq 'args=--force-source --input future.bin' <<< "$output" ||
  fail "future-source arguments were not forwarded unchanged"
grep -Fq 'NXExtract using the X5M firmware SDL2/KMSDRM scope' <<< "$output" ||
  fail "the proven X5M route was not selected"
grep -Fq '/game-only/leak' <<< "$output" &&
  fail "an inherited game library path leaked into NXExtract"

grep -Fq \
  'summertime_run_extractor "$GAMEDIR" "$controlfolder" / "$machine"' \
  "$OUTER_SH" ||
  fail "the public launcher does not call the scoped extractor helper"
grep -Fq \
  'summertime_run_extractor "$GAME_DIR" "$controlfolder" / "$machine"' \
  "$UPDATE_SH" ||
  fail "the future-data updater does not call the scoped extractor helper"

if grep -Eq \
  '^[[:space:]]*set[[:space:]]+(-[A-Za-z]*u[A-Za-z]*|-o[[:space:]]+nounset)([[:space:]]|$)' \
  "$OUTER_SH"; then
  fail "the PortMaster-facing launcher enables nounset before control.txt"
fi
if grep -En \
  '(^|[[:space:]])(setsid|nohup|systemctl[[:space:]]+(stop|mask|restart))([[:space:]]|$)' \
  "$OUTER_SH" "$ENV_SH" "$UPDATE_SH"; then
  fail "a launcher lifecycle command entered the public extractor path"
fi
if grep -En \
  '^[[:space:]]*(export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER=' \
  "$OUTER_SH" "$ENV_SH" "$UPDATE_SH"; then
  fail "the public extractor path forces a display or audio backend"
fi

printf 'Summertime X5M extractor scope test: PASS\n'
