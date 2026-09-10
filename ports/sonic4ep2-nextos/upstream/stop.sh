#!/bin/sh
set -eu

PORT_DIR="${SONIC_PORT_DIR:-/storage/roms/ports/sonic4}"

kill_existing_sonic4() {
  self="$$"

  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$PORT_DIR"/sonic4*)
        echo "killing sonic4 pid $pid: $target"
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
        echo "force killing sonic4 pid $pid: $target"
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done
}

cd "$PORT_DIR"
kill_existing_sonic4
