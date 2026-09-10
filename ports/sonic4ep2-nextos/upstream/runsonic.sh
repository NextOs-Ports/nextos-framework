#!/bin/sh
set -eu

PORT_DIR="${SONIC_PORT_DIR:-/storage/roms/ports/sonic4}"
SECONDS_TO_RUN="${1:-95}"

kill_existing_sonic4() {
  self="$$"

  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$PORT_DIR"/sonic4*)
        echo "killing stale sonic4 pid $pid: $target"
        kill "$pid" 2>/dev/null || true
        ;;
    esac
  done

  sleep 1

  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$PORT_DIR"/sonic4*)
        echo "force killing stale sonic4 pid $pid: $target"
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done
}

cd "$PORT_DIR"
kill_existing_sonic4

export SONIC_DATADIR="${SONIC_DATADIR:-$PORT_DIR}"
export SONIC_AUTOSTART="${SONIC_AUTOSTART:-1}"
export SONIC_NOFAKESOUND="${SONIC_NOFAKESOUND:-1}"
export SONIC_SWAPINT="${SONIC_SWAPINT:-1}"

env ${SONIC_EXTRA:-} ./sonic4 > play.log 2>&1 &
GPID="$!"
echo "launched sonic4 pid $GPID, waiting ${SECONDS_TO_RUN}s..."
sleep "$SECONDS_TO_RUN"

rm -f /dev/shm/sonic_shot.raw /dev/shm/sonic_shot.txt
touch /dev/shm/sonic_shot
sleep 2
ls -la /dev/shm/sonic_shot.raw 2>/dev/null || true
echo "=== last frame in log ==="
grep -o '\[frame [0-9]*\]' play.log | tail -1 || true
