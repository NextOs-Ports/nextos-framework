#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Create one append-only source checkpoint only after every earlier safe gate passed.
set -euo pipefail

REPOSITORY=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
CAPTURE=$REPOSITORY/framework/tools/capture-checkpoint.sh

if [[ ${1:-} != --checkpoint-root || -z ${2:-} || $# -ne 2 ]]; then
  printf 'usage: %s --checkpoint-root ABSOLUTE_DIRECTORY\n' "${0##*/}" >&2
  exit 2
fi
checkpoint_root=$2
case $checkpoint_root in
  /*) ;;
  *)
    printf 'capture-green-checkpoint: checkpoint root must be absolute\n' >&2
    exit 2
    ;;
esac

exec "$CAPTURE" \
  --repo "$REPOSITORY" \
  --checkpoint-root "$checkpoint_root" \
  -- framework suportando_outros_devices publicando_ports
