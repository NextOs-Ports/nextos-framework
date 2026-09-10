#!/usr/bin/env bash
# Test-only frontend wrapper. It is transferred to the device staging area,
# used for one bounded proof, then removed; it is not part of the public ZIP.
if false; then
  GAMEDIR="/$directory/ports/sallyface"
fi
export SF_GPVIRT=1
export SF_GPTK_PROOF=1
exec "$(dirname -- "$0")/Sally Face.sh" "$@"
