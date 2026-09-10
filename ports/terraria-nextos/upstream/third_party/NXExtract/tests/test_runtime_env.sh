#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxextract-runtime-env.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

fail() {
  printf 'runtime environment test: %s\n' "$*" >&2
  exit 1
}

GAME_DIR="$TEST_ROOT/game data"
GAME_ALIAS="$TEST_ROOT/game-alias"
FIRMWARE_DIR="$TEST_ROOT/firmware-sdl"
INHERITED_DIR="$TEST_ROOT/inherited-system"
OUTSIDE_PRIVATE_DIR="$TEST_ROOT/outside-private"
mkdir -p \
  "$GAME_DIR/libs" \
  "$GAME_DIR/private" \
  "$FIRMWARE_DIR" \
  "$INHERITED_DIR" \
  "$OUTSIDE_PRIVATE_DIR"
ln -s "$GAME_DIR" "$GAME_ALIAS"
ln -s "$OUTSIDE_PRIVATE_DIR" "$GAME_DIR/private-link"

CAPTURE="$TEST_ROOT/capture"
PROBE="$TEST_ROOT/probe.sh"
cat >"$PROBE" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
{
  printf 'ld=%s\n' "${LD_LIBRARY_PATH:-}"
  printf 'sdl=%s\n' "${SDL_VIDEODRIVER-__UNSET__}"
  printf 'sdl_legacy=%s\n' "${SDL_VIDEO_DRIVER-__UNSET__}"
  printf 'sdl_audio=%s\n' "${SDL_AUDIODRIVER-__UNSET__}"
  printf 'active=%s\n' "${NXEXTRACT_RUNTIME_ENV_ACTIVE:-0}"
  printf 'args=%s\n' "$*"
} >"$TEST_CAPTURE"
EOF
chmod +x "$PROBE"

original_ld="$GAME_DIR/libs:$INHERITED_DIR:$GAME_DIR/private:$GAME_ALIAS/libs:$GAME_DIR/private-link"
NXEXTRACT_GAME_DIR="$GAME_DIR" \
NXEXTRACT_FIRMWARE_LIBRARY_PATH="$FIRMWARE_DIR:$GAME_DIR/libs" \
NXEXTRACT_MACHINE=aarch64 \
LD_LIBRARY_PATH="$original_ld" \
SDL_VIDEODRIVER=wayland \
SDL_VIDEO_DRIVER=wayland-legacy \
SDL_AUDIODRIVER=pulseaudio \
TEST_CAPTURE="$CAPTURE" \
  "$PROJECT_ROOT/nxextract-runtime-env.sh" "$PROBE" direct probe

direct_ld=$(sed -n 's/^ld=//p' "$CAPTURE")
[ "$(sed -n 's/^sdl=//p' "$CAPTURE")" = wayland ] ||
  fail "the inherited SDL backend was changed"
[ "$(sed -n 's/^sdl_legacy=//p' "$CAPTURE")" = wayland-legacy ] ||
  fail "the inherited legacy SDL backend was changed"
[ "$(sed -n 's/^sdl_audio=//p' "$CAPTURE")" = pulseaudio ] ||
  fail "the inherited SDL audio backend was changed"
[ "$(sed -n 's/^active=//p' "$CAPTURE")" = 1 ] ||
  fail "the child scope was not marked active"
[ "$(sed -n 's/^args=//p' "$CAPTURE")" = 'direct probe' ] ||
  fail "the wrapped command arguments changed"
case ":$direct_ld:" in
  *":$GAME_DIR:"*|*":$GAME_DIR/"*) fail "a game-private path leaked" ;;
esac
case ":$direct_ld:" in
  *":$GAME_ALIAS:"*|*":$GAME_ALIAS/"*) fail "a symlinked game-private path leaked" ;;
esac
case ":$direct_ld:" in
  *":$OUTSIDE_PRIVATE_DIR:"*) fail "an outward game-private symlink leaked" ;;
esac
case ":$direct_ld:" in
  *":$FIRMWARE_DIR:"*) ;;
  *) fail "the explicit firmware path is missing" ;;
esac
case ":$direct_ld:" in
  *":$INHERITED_DIR:"*) ;;
  *) fail "the safe inherited system path is missing" ;;
esac
case "$direct_ld" in
  *"$FIRMWARE_DIR"*"$INHERITED_DIR"*) ;;
  *) fail "firmware paths do not precede inherited paths" ;;
esac

NXEXTRACT_GAME_DIR="$GAME_DIR" \
NXEXTRACT_PYTHON="$PROBE" \
NXEXTRACT_FIRMWARE_LIBRARY_PATH="$FIRMWARE_DIR" \
NXEXTRACT_MACHINE=aarch64 \
LD_LIBRARY_PATH="$GAME_DIR/libs:$INHERITED_DIR" \
SDL_VIDEODRIVER=kmsdrm \
TEST_CAPTURE="$CAPTURE" \
  "$PROJECT_ROOT/run-extractor.sh" --force-source --input renamed.apk

[ "$(sed -n 's/^sdl=//p' "$CAPTURE")" = kmsdrm ] ||
  fail "run-extractor changed the inherited SDL backend"
[ "$(sed -n 's/^active=//p' "$CAPTURE")" = 1 ] ||
  fail "run-extractor bypassed the isolated child scope"
case "$(sed -n 's/^args=//p' "$CAPTURE")" in
  *nxextract.py*install*--force-source*--input*renamed.apk*) ;;
  *) fail "run-extractor did not preserve the CLI arguments" ;;
esac

unset SDL_VIDEODRIVER SDL_VIDEO_DRIVER SDL_AUDIODRIVER
NXEXTRACT_GAME_DIR="$GAME_DIR" \
NXEXTRACT_MACHINE=aarch64 \
TEST_CAPTURE="$CAPTURE" \
  "$PROJECT_ROOT/nxextract-runtime-env.sh" "$PROBE" autodetect
[ "$(sed -n 's/^sdl=//p' "$CAPTURE")" = __UNSET__ ] ||
  fail "an SDL backend was forced when autodetection was requested"

NXEXTRACT_GAME_DIR="$GAME_DIR" \
NXEXTRACT_MACHINE=aarch64 \
NXEXTRACT_SDL_AUTODETECT=1 \
SDL_VIDEODRIVER=invalid-video \
SDL_VIDEO_DRIVER=invalid-legacy-video \
SDL_AUDIODRIVER=invalid-audio \
TEST_CAPTURE="$CAPTURE" \
  "$PROJECT_ROOT/nxextract-runtime-env.sh" "$PROBE" explicit-autodetect
[ "$(sed -n 's/^sdl=//p' "$CAPTURE")" = __UNSET__ ] ||
  fail "the explicit video autodetection opt-in was ignored"
[ "$(sed -n 's/^sdl_legacy=//p' "$CAPTURE")" = __UNSET__ ] ||
  fail "the explicit legacy-video autodetection opt-in was ignored"
[ "$(sed -n 's/^sdl_audio=//p' "$CAPTURE")" = __UNSET__ ] ||
  fail "the explicit audio autodetection opt-in was ignored"

printf 'NXExtract runtime environment tests passed\n'
