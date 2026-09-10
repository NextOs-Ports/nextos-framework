#!/usr/bin/env bash
# Launch the installed candidate remotely without inventing a timeout/exit path.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
DEV=${DEV:?set DEV to the IP authorized for this session}
SSH_USER=${SSH_USER:-root}
REMOTE=${SSH_USER}@${DEV}
SCRIPT=${SC_INPUTTEST:-}
SHOTS=${SC_SCREENSHOT_FRAMES:-}

scp -q -o BatchMode=yes "$PORT_DIR/tools/launch_device.sh" \
  "$REMOTE:/tmp/suzycube-launch-device.sh"
timeout 60 ssh -o BatchMode=yes "$REMOTE" \
  "chmod 0755 /tmp/suzycube-launch-device.sh && /tmp/suzycube-launch-device.sh '$SCRIPT' '$SHOTS'"
printf 'Suzy Cube launch requested on the explicitly authorized device %s\n' "$DEV"
