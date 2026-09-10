#!/bin/bash
# cycle.sh -- build, envia o loader e roda no aparelho, devolvendo o log filtrado.
# Uso: devtools/cycle.sh [segundos] [extra_env]
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
DEV="${BT_DEVICE:?defina BT_DEVICE}"
SECS="${1:-60}"; EXTRA="${2:-}"
bash build.sh >/dev/null
timeout 300 rsync -rlt --no-perms --no-owner --no-group blossomtales run_device.sh "root@$DEV:/storage/roms/ports/blossomtales/"
timeout $((SECS + 180)) ssh "root@$DEV" \
  "cd /storage/roms/ports/blossomtales && chmod +x blossomtales run_device.sh && \
   SB_RUN_SECONDS=$SECS SB_TAIL=1 SB_EXTRA_ENV='$EXTRA' nice -n 19 ./run_device.sh >/dev/null 2>&1; \
   grep -vE 'so_load:|init\[|so_execute_init|\[so_dlopen\]|\[alog\] Could not load|\[prop\]' debug.log | tail -n \${BT_TAIL:-70}"
