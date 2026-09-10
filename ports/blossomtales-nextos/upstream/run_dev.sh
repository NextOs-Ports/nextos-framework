#!/bin/bash
# run_dev.sh -- iteracao rapida: builda no host, sincroniza o loader e roda no
# aparelho com log persistente. O IP vem de SB_DEVICE (nunca fica no repo).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEV="${SB_DEVICE:?defina SB_DEVICE=<ip do aparelho>}"
GAMEDIR=/storage/roms/ports/blossomtales
SECS="${1:-90}"

bash "$HERE/build.sh"
nice -n 19 rsync -a "$HERE/blossomtales" "$HERE/run_device.sh" root@"$DEV":"$GAMEDIR"/
nice -n 19 timeout $((SECS + 60)) ssh root@"$DEV" \
  "cd $GAMEDIR && nice -n 19 SB_RUN_SECONDS=$SECS bash run_device.sh"
