#!/usr/bin/env bash
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# shellcheck source=../nxextract-runtime-env.sh
source "$REPO/nxextract-runtime-env.sh"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sdv-runtime-test.XXXXXX")
cleanup() {
  rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM

fail() {
  printf 'runtime environment test failed: %s\n' "$*" >&2
  exit 1
}

FAKE_ROOT="$TMP_ROOT/root"
X5M_ROOT="$TMP_ROOT/x5m-root"
GAME="$TMP_ROOT/game"
CONTROL="$TMP_ROOT/control"
mkdir -p "$FAKE_ROOT/etc" "$X5M_ROOT/etc" \
  "$X5M_ROOT/proc/device-tree" "$GAME/.nxextract/stardewvalley-nextos" \
  "$CONTROL/libs" "$CONTROL/libs.aarch64"
printf 'ID=rocknix\nNAME=ROCKNIX\n' > "$FAKE_ROOT/etc/os-release"
printf 'ID=nextos\nNAME=NextOS\n' > "$X5M_ROOT/etc/os-release"
printf 'amlogic,s905x5m\000' > "$X5M_ROOT/proc/device-tree/compatible"

cat > "$GAME/run-extractor.sh" <<'SH'
#!/bin/sh
printf '%s\n' "${LD_LIBRARY_PATH:-}" > "$TEST_LD_CAPTURE"
printf '%s\n' "${SDL_VIDEODRIVER:-}" > "$TEST_SDL_CAPTURE"
printf '%s\n' "${SDL_VIDEO_DRIVER:-}" > "$TEST_SDL_ALT_CAPTURE"
printf '%s\n' "${NXEXTRACT_GAME_DIR:-}" > "$TEST_GAME_CAPTURE"
if [ -n "${TEST_UI_APPEND:-}" ]; then
  printf '%s\n' "$TEST_UI_APPEND" >> \
    "$NXEXTRACT_GAME_DIR/.nxextract/stardewvalley-nextos/ui.log"
fi
exit "${TEST_EXIT:-0}"
SH
chmod +x "$GAME/run-extractor.sh"

export TEST_LD_CAPTURE="$TMP_ROOT/extractor-ld.txt"
export TEST_SDL_CAPTURE="$TMP_ROOT/extractor-sdl.txt"
export TEST_SDL_ALT_CAPTURE="$TMP_ROOT/extractor-sdl-alt.txt"
export TEST_GAME_CAPTURE="$TMP_ROOT/extractor-game.txt"

(
  export LD_LIBRARY_PATH=/bad/inherited/path
  export SDL_VIDEODRIVER=wayland
  export SDL_VIDEO_DRIVER=wayland-alt
  sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" aarch64 >/dev/null
  [ "$LD_LIBRARY_PATH" = /bad/inherited/path ] ||
    fail "extractor LD_LIBRARY_PATH leaked into launcher"
  [ "$SDL_VIDEODRIVER" = wayland ] ||
    fail "extractor SDL_VIDEODRIVER leaked into launcher"
)

expected_ld="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$CONTROL/libs:$CONTROL/libs.aarch64:/bad/inherited/path"
[ "$(< "$TEST_LD_CAPTURE")" = "$expected_ld" ] ||
  fail "AArch64 firmware/control/inherited library order is wrong"
[ "$(< "$TEST_SDL_CAPTURE")" = wayland ] ||
  fail "non-X5M inherited SDL backend was not preserved"
[ "$(< "$TEST_SDL_ALT_CAPTURE")" = wayland-alt ] ||
  fail "non-X5M alternate SDL backend was not preserved"
[ "$(< "$TEST_GAME_CAPTURE")" = "$GAME" ] ||
  fail "NXEXTRACT_GAME_DIR was not scoped to the extractor"

# muOS/ArkOS-style KMSDRM selections are firmware decisions too: the generic
# AArch64 path must not replace them with Wayland or any hardcoded backend.
(
  export LD_LIBRARY_PATH=/muos/inherited
  export SDL_VIDEODRIVER=kmsdrm
  export SDL_VIDEO_DRIVER=kmsdrm-alt
  sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" aarch64 >/dev/null
)
[ "$(< "$TEST_SDL_CAPTURE")" = kmsdrm ] ||
  fail "generic AArch64 KMSDRM backend was not preserved"
[ "$(< "$TEST_SDL_ALT_CAPTURE")" = kmsdrm-alt ] ||
  fail "generic AArch64 alternate KMSDRM backend was not preserved"

# With no firmware choice, leave both variables empty so SDL can probe the
# device instead of inheriting a policy invented by the port.
(
  export LD_LIBRARY_PATH=/arkos/inherited
  unset SDL_VIDEODRIVER SDL_VIDEO_DRIVER
  sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" arm64 >/dev/null
)
[ -z "$(< "$TEST_SDL_CAPTURE")" ] ||
  fail "generic AArch64 SDL autodetection was overridden"
[ -z "$(< "$TEST_SDL_ALT_CAPTURE")" ] ||
  fail "generic AArch64 alternate SDL autodetection was overridden"

(
  export LD_LIBRARY_PATH=/inherited
  export SDL_VIDEODRIVER=kmsdrm
  export SDL_VIDEO_DRIVER=kmsdrm-alt
  sdv_run_extractor "$GAME" "$CONTROL" "$X5M_ROOT" arm64 >/dev/null
)
[ -z "$(< "$TEST_SDL_CAPTURE")" ] ||
  fail "X5M SDL_VIDEODRIVER autodetection was not enabled"
[ -z "$(< "$TEST_SDL_ALT_CAPTURE")" ] ||
  fail "X5M SDL_VIDEO_DRIVER autodetection was not enabled"

(
  export LD_LIBRARY_PATH=/native/unchanged
  export SDL_VIDEODRIVER=x11
  sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" x86_64 >/dev/null
)
[ "$(< "$TEST_LD_CAPTURE")" = /native/unchanged ] ||
  fail "non-AArch64 extractor environment was modified"
[ "$(< "$TEST_SDL_CAPTURE")" = x11 ] ||
  fail "non-AArch64 SDL backend was modified"

ui_log="$GAME/.nxextract/stardewvalley-nextos/ui.log"
printf 'OLD DIAGNOSTIC\n' > "$ui_log"
unset TEST_UI_APPEND
output=$(sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" aarch64)
case "$output" in
  *'NXExtract UI diagnostic'*|*'OLD DIAGNOSTIC'*)
    fail "unchanged NXExtract UI log was repeated" ;;
esac

export TEST_UI_APPEND='NEW DIAGNOSTIC'
output=$(sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" aarch64)
case "$output" in *'NEW DIAGNOSTIC'*) ;; *)
  fail "new NXExtract UI diagnostic was not copied" ;;
esac
case "$output" in *'OLD DIAGNOSTIC'*)
  fail "stale NXExtract UI diagnostic was copied with the new entry" ;;
esac
unset TEST_UI_APPEND

export TEST_EXIT=23
set +e
sdv_run_extractor "$GAME" "$CONTROL" "$FAKE_ROOT" aarch64 >/dev/null
status=$?
set -e
unset TEST_EXIT
[ "$status" -eq 23 ] || fail "extractor exit status was not preserved"

printf 'runtime environment tests: PASS\n'
