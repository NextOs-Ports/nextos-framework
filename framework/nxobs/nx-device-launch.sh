#!/usr/bin/env bash
# Launch a port on a device the way its frontend would, and report the verdict.
#
# A port started from a plain SSH shell does not inherit what the frontend's
# service unit declares, and on ArkOS/dArkOS that difference alone is the
# difference between a rendered game and a black screen: the unit carries
#
#   Environment="SDL_VIDEO_EGL_DRIVER=libEGL.so"
#
# because the firmware's versioned SONAMEs (libEGL.so.1, libGLESv1_CM.so.1)
# resolve to driverless stubs while the unversioned names are the real Mali
# blob. Without it SDL_CreateWindow fails outright, the GL provider repair
# takes its pre-context branch, every candidate reports "eglInitialize failed
# on this kernel", and the run looks exactly like a broken port. It is not.
#
# So this harness does not hardcode that variable. It reads the frontend unit
# on the device and reproduces whatever that unit declares, which keeps working
# when a firmware declares something else.
#
# Usage:
#   nx-device-launch.sh --host IP --user USER [--password PW]
#                       --launcher "/roms/ports/Swordigo.sh"
#                       [--seconds 40] [--keep-running]
set -euo pipefail

export LC_ALL=C

# Onda v2 (AUD-10): a senha preferencialmente vem de SSHPASS (env); --password
# continua aceito por compat, mas argv vaza em /proc/*/cmdline.
HOST="" USER_NAME="" PASSWORD="${SSHPASS:-}" LAUNCHER="" SECONDS_TO_RUN=40 KEEP_RUNNING=0
FRONTEND_STOP_CMD="" FRONTEND_START_CMD=""

fail() { printf 'nx-device-launch: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case $1 in
    --host) HOST=${2:-}; shift 2 ;;
    --user) USER_NAME=${2:-}; shift 2 ;;
    --password) PASSWORD=${2:-}; shift 2 ;;
    --frontend-stop) FRONTEND_STOP_CMD=${2:-}; shift 2 ;;
    --frontend-start) FRONTEND_START_CMD=${2:-}; shift 2 ;;
    --launcher) LAUNCHER=${2:-}; shift 2 ;;
    --seconds) SECONDS_TO_RUN=${2:-}; shift 2 ;;
    --keep-running) KEEP_RUNNING=1; shift ;;
    *) fail "unknown argument: $1" ;;
  esac
done

[ -n "$HOST" ] || fail 'missing --host'
[ -n "$USER_NAME" ] || fail 'missing --user'
[ -n "$LAUNCHER" ] || fail 'missing --launcher'
case $SECONDS_TO_RUN in
  ''|*[!0-9]*) fail '--seconds must be a whole number' ;;
esac
# The frame proof samples at frames 300/600/900, so a shorter run can only
# ever produce an UNKNOWN verdict.
[ "$SECONDS_TO_RUN" -ge 15 ] || fail '--seconds must be at least 20'

# The IP is a runtime argument on purpose: device addresses are never baked in.
case $HOST in
  *[!0-9.]*) fail 'refusing a non-literal host: pass the current device IP' ;;
esac

ssh_device() {
  if [ -n "$PASSWORD" ]; then
    SSHPASS="$PASSWORD" sshpass -e ssh -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -n \
      "$USER_NAME@$HOST" "$1"
  else
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -n \
      "$USER_NAME@$HOST" "$1"
  fi
}

PORT_DIR=$(dirname -- "$LAUNCHER")
GAME_ID=$(basename -- "$LAUNCHER" .sh)

printf 'nx-device-launch: discovering the frontend on %s\n' "$HOST"

# Minimal firmware images ship no sudo and log in as root already.
SUDO=$(ssh_device '[ "$(id -u)" = 0 ] && echo "" || echo sudo') ||
  fail 'cannot reach the device'

# Frontend unit names differ per firmware; ask the device instead of guessing.
# list-unit-files still lists a masked unit, which `systemctl cat` does not:
# the frontend is deliberately masked while a port is under test, and that must
# not read as "this device has no frontend".
# A device can carry more than one frontend unit -- an Amlogic image keeps a
# masked legacy emustation next to the essway it actually boots -- so pick by
# evidence rather than by a fixed order: a unit whose ExecStart target is
# missing is a leftover, and an active or enabled unit beats a disabled one.
FRONTEND=$(ssh_device "
  best=''; best_score=-1
  for unit in emulationstation.service emustation.service essway.service; do
    $SUDO systemctl list-unit-files \"\$unit\" >/dev/null 2>&1 || continue
    $SUDO systemctl list-unit-files \"\$unit\" 2>/dev/null | grep -q \"^\$unit\" || continue
    exec_start=\$($SUDO systemctl cat \"\$unit\" 2>/dev/null |
      sed -n 's/^ExecStart=//p' | head -1 | awk '{print \$1}')
    score=0
    [ -n \"\$exec_start\" ] && [ -x \"\$exec_start\" ] && score=\$((score + 2))
    state=\$($SUDO systemctl is-enabled \"\$unit\" 2>/dev/null)
    [ \"\$state\" = enabled ] && score=\$((score + 4))
    [ \"\$state\" = masked ] && score=\$((score - 4))
    $SUDO systemctl is-active \"\$unit\" >/dev/null 2>&1 && score=\$((score + 8))
    if [ \"\$score\" -gt \"\$best_score\" ]; then best=\$unit; best_score=\$score; fi
  done
  echo \"\$best\"
") || fail 'cannot reach the device'
if [ -z "$FRONTEND" ] && [ -n "$FRONTEND_STOP_CMD" ] && \
   [ -n "$FRONTEND_START_CMD" ]; then
  # Onda v2 (AUD-30): firmwares sem systemd (muOS/spruce/CrossMix) abortavam
  # aqui e ficavam sem caminho de prova. O par stop/start vem de quem MEDIU o
  # aparelho, por argumento explicito -- nunca inventado por nome de CFW. O
  # ambiente do frontend, nesses casos, e' o do proprio processo de boot.
  FRONTEND="explicit-command"
  printf 'nx-device-launch: frontend without systemd; using the measured stop/start pair\n'
fi
[ -n "$FRONTEND" ] || fail 'no known frontend unit on this device (pass --frontend-stop/--frontend-start measured on it)'
printf 'nx-device-launch: frontend unit is %s\n' "$FRONTEND"

# Reproduce exactly what the frontend declares. This is not "forcing an SDL
# driver": it is refusing to launch in an environment the firmware never uses.
if [ "$FRONTEND" = explicit-command ]; then
  # Sem unit para ler: o ambiente do frontend e' o do proprio boot da CFW.
  FRONTEND_ENV=""
  FRONTEND_USER=$USER_NAME
else
FRONTEND_ENV=$(ssh_device "$SUDO systemctl cat $FRONTEND 2>/dev/null |
  sed -n 's/^Environment=\"\\(.*\\)\"\$/\\1/p; s/^Environment=\\([^\"].*\\)\$/\\1/p' |
  tr '\n' ' '")
FRONTEND_USER=$(ssh_device "$SUDO systemctl cat $FRONTEND 2>/dev/null |
  sed -n 's/^User=//p' | head -1")
fi
[ -n "$FRONTEND_USER" ] || FRONTEND_USER=$USER_NAME

# Ask for the home directory instead of assuming /home/<user>: root is /root on
# one image and /storage on another, and a wrong HOME silently changes where the
# firmware looks for its own identity files.
FRONTEND_HOME=$(ssh_device "getent passwd '$FRONTEND_USER' 2>/dev/null |
  cut -d: -f6") || true
[ -n "$FRONTEND_HOME" ] || FRONTEND_HOME=$(ssh_device "
  $SUDO systemctl cat $FRONTEND 2>/dev/null | sed -n 's/^WorkingDirectory=//p' |
  head -1") || true
[ -n "$FRONTEND_HOME" ] || fail "cannot resolve the home of $FRONTEND_USER"

if [ -n "$FRONTEND_ENV" ]; then
  printf 'nx-device-launch: inheriting from the unit: %s\n' "$FRONTEND_ENV"
else
  printf 'nx-device-launch: the unit declares no environment\n'
fi

PROOF_DIR=""
PROOF_DIR_OWNED=0

cleanup_proof_dir() {
  if [ "$PROOF_DIR_OWNED" = 1 ]; then
    ssh_device "rm -rf '$PROOF_DIR'" >/dev/null 2>&1 || true
    PROOF_DIR_OWNED=0
  fi
}

restore_frontend() {
  cleanup_proof_dir
  printf 'nx-device-launch: restoring %s\n' "$FRONTEND"
  if [ "$FRONTEND" = explicit-command ]; then ssh_device "$FRONTEND_START_CMD"; else ssh_device "$SUDO systemctl start $FRONTEND"; fi >/dev/null 2>&1 || true
}
trap restore_frontend EXIT INT TERM

printf 'nx-device-launch: stopping the frontend and launching %s\n' "$GAME_ID"

# Nothing of ours may already be running: a second instance fights for the
# display and both look broken. Match by executable directory, never by name.
LIVE=$(ssh_device "$SUDO ls -l /proc/[0-9]*/exe 2>/dev/null |
  grep -c '$PORT_DIR/' || true")
[ "${LIVE:-0}" = "0" ] || fail "something is already running under $PORT_DIR"

if [ "$FRONTEND" = explicit-command ]; then ssh_device "$FRONTEND_STOP_CMD"; else ssh_device "$SUDO systemctl stop $FRONTEND"; fi >/dev/null 2>&1 || true

# env -i so the run carries the frontend's environment and nothing else: an
# SSH login otherwise leaks SSH_*, XDG_* and DBUS_* into a session that the
# frontend never has.
# The loader writes its proof image here when asked. A frame the loader read
# back with glReadPixels is the only capture that works on every firmware: on
# RK3326 the DRM buffer the game presents cannot be mapped from outside.
PROOF_DIR="/tmp/nx-proof-$$"
# A single mkdir is the ownership claim: an existing directory or symlink is
# refused, never reused or removed. umask 077 makes the new directory private
# without a chmod race; the frame-proof adapter independently rejects a mode
# that the runtime filesystem did not honor.
if ! ssh_device "umask 077 && mkdir '$PROOF_DIR'"; then
  fail "cannot create an exclusive private proof directory at $PROOF_DIR"
fi
PROOF_DIR_OWNED=1

LAUNCH_CMD="cd '$PORT_DIR' && env -i HOME=$FRONTEND_HOME \
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin TERM=linux \
USER=$FRONTEND_USER NXLAUNCH_PROOF_DIR='$PROOF_DIR' $FRONTEND_ENV bash '$LAUNCHER'"

if [ "$KEEP_RUNNING" = 1 ]; then
  # Requested for hands-on play: no timeout, and it outlives this shell.
  ssh_device "nohup sh -c \"$LAUNCH_CMD\" >/dev/null 2>&1 &" || true
  printf 'nx-device-launch: launched and left running on the device\n'
  trap - EXIT INT TERM
  exit 0
fi

ssh_device "timeout -s KILL $SECONDS_TO_RUN sh -c \"$LAUNCH_CMD\"" \
  >/dev/null 2>&1 || true

ssh_device "$SUDO ls -l /proc/[0-9]*/exe 2>/dev/null |
  grep '$PORT_DIR/' | sed -E 's#.*/proc/([0-9]+)/exe.*#\1#' |
  while read -r pid; do $SUDO kill -TERM \"\$pid\"; done" >/dev/null 2>&1 || true

# The launcher file name is not the port directory name: "Hitman GO.sh" keeps
# its data in "hitmango", and a FAT card hides a case difference that an ext4
# rootfs does not. The generated launcher states the directory itself, so read
# it from there instead of guessing from the file name.
# nxbootstrap 0.7.x launchers declare NXBOOTSTRAP_LOGICAL_GAMEDIR; older ones GAMEDIR.
GAME_DIR=$(ssh_device "sed -n -E 's|^(NXBOOTSTRAP_LOGICAL_GAMEDIR\|GAMEDIR)=\"/\$directory/ports/([A-Za-z0-9._-]*)\".*|\\2|p' '$LAUNCHER' | head -1") || true
if [ -n "$GAME_DIR" ]; then
  GAME_DIR="$PORT_DIR/$GAME_DIR"
else
  # Fall back to a case-insensitive match on a directory that holds a log.
  GAME_DIR=$(ssh_device "
    for d in '$PORT_DIR'/*/; do
      [ -f \"\$d/log.txt\" ] || continue
      case \$(basename \"\$d\" | tr 'A-Z' 'a-z') in
        \$(echo '$GAME_ID' | tr 'A-Z' 'a-z')) printf '%s' \"\${d%/}\"; exit 0 ;;
      esac
    done
    printf ''
  ") || true
fi
[ -n "$GAME_DIR" ] || GAME_DIR="$PORT_DIR/$GAME_ID"
LOG_PATH="$GAME_DIR/log.txt"

# The launcher writes the log through a redirect that only settles once the
# process is gone, so reading immediately after the kill can catch an empty
# tail and turn a good run into a false INCONCLUSIVE. Bounded retry, never a
# wait that cannot end.
VERDICT=""
for _ in 1 2 3 4 5; do
  VERDICT=$(ssh_device "sleep 2; grep -o 'frame proof verdict=[A-Z]*' \
    '$LOG_PATH' 2>/dev/null | tail -1 | cut -d= -f2") || true
  [ -z "$VERDICT" ] || break
done

# Pull the proof image and measure it. A capture only counts when it has real
# content -- mean colour and distinct colours -- the same rule that exposed
# twelve empty screenshots filed as gameplay proof.
IMAGE_STATUS="none"
if [ -n "${NX_PROOF_OUT:-}" ]; then
  mkdir -p "$NX_PROOF_OUT"
  local_png="$NX_PROOF_OUT/$(printf '%s' "$HOST" | tr '.' '-')--$GAME_ID.png"
  if [ -n "$PASSWORD" ]; then
    SSHPASS="$PASSWORD" sshpass -e scp -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
      "$USER_NAME@$HOST:$PROOF_DIR/frame-proof.png" "$local_png" 2>/dev/null || true
  else
    scp -o BatchMode=yes -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
      "$USER_NAME@$HOST:$PROOF_DIR/frame-proof.png" "$local_png" 2>/dev/null || true
  fi
  if [ -s "$local_png" ]; then
    IMAGE_STATUS=$(python3 -c '
import sys
try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("unmeasured"); sys.exit(0)
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
cores = len(np.unique(a.reshape(-1, 3), axis=0))
# Empty means one flat colour (or a couple of near-black bands): the twelve
# fake captures all had 1-3 colours and a mean under 0.1. A retro title card
# in five colours on black is a real frame -- RCR DX proved that -- so the
# colour floor is 4, not a number that quietly rejects pixel art.
ok = a.mean() >= 3.0 and cores >= 4
print(("real" if ok else "EMPTY") + " mean=%.1f colours=%d" % (a.mean(), cores))
' "$local_png")
    printf 'nx-device-launch: proof image %s (%s)\n' "$local_png" "$IMAGE_STATUS"
  else
    printf 'nx-device-launch: proof image not produced\n'
  fi
fi
cleanup_proof_dir

printf '\nnx-device-launch: verdict\n\'
ssh_device "grep -E '^launch:|renderer=|frame proof|FATAL' \
  '$LOG_PATH' 2>/dev/null | tail -6" || true

case ${VERDICT:-} in
  OK)
    case $IMAGE_STATUS in
      EMPTY*) printf '\nnx-device-launch: FAIL — verdict says OK but the proof image is empty\n'; exit 1 ;;
    esac
    printf '\nnx-device-launch: PASS — the port drew a real frame\n'; exit 0 ;;
  BLACK) printf '\nnx-device-launch: FAIL — measured black on a launch that could draw\n'; exit 1 ;;
  INCONCLUSIVE|UNKNOWN|UNMEASURED|'')
    printf '\nnx-device-launch: INCONCLUSIVE — nothing was proven, do not report this as a defect\n'
    exit 2 ;;
  *) printf '\nnx-device-launch: unrecognized verdict %s\n' "$VERDICT"; exit 2 ;;
esac
