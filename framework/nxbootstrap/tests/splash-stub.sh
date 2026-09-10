#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Host-side test double for an architecture-specific nxsplash ELF.
set -eu
[ "$#" -eq 1 ]
if [ -n "${NXBOOTSTRAP_TEST_SPLASH_ORDER:-}" ]; then
  printf 'splash:%s\n' "$1" >> "$NXBOOTSTRAP_TEST_SPLASH_ORDER"
fi
exit "${NXBOOTSTRAP_TEST_SPLASH_STATUS:-0}"
