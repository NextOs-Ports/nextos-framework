#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Static gate: nxinput must never select behavior by device name, CFW label,
# firmware string or a hard-coded VID/PID. Those are diagnostic facts owned by
# the PortMaster/firmware mapping and by nxcompat, never by this module.
#
# Arguments:
#   $1 - nxinput source root (defaults to the script's own directory/.. )
#
# Exits 0 when the scanned sources are clean, 1 on the first forbidden token.

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$#" -ge 1 ] && [ -n "${1:-}" ]; then
  root="$1"
fi

scan_dir="$root/src"
header_dir="$root/include"
if [ ! -d "$scan_dir" ] || [ ! -d "$header_dir" ]; then
  echo "static-no-device-name-fallback: source tree not found under '$root'" >&2
  exit 1
fi

# Lower-cased substrings that would betray a name/firmware/VID-PID fallback.
denylist=(
  'nextos' 'arkos' 'rocknix' 'knulli' 'batocera' 'muos' 'miyoo' 'trimui'
  'retrodeck' 'emuelec' 'darkos' 'jelos' 'panfrost' 'odroid'
  'gameforce' 'anbernic' 'powkiddy' 'mali-450' 'x5m'
  '0810:0001' '054c:' '0b05:' '0e6f:' '28de:'
)

status=0
for token in "${denylist[@]}"; do
  # -I so grep treats the pattern as fixed; case-insensitive.
  matches=$(grep -rIn --include='*.c' --include='*.h' -i -F -- "$token" \
       "$scan_dir" "$header_dir" || true)
  if [ "$token" = "nextos" ] && [ -n "$matches" ]; then
    # V3 exception, deliberately narrow: the NEXTOSCONTROLLERS.gptk format
    # name and its `authority = nextos|native` value name WHO shapes the
    # axes (the framework itself vs the game) -- an owner-facing semantic
    # constant, not a device/CFW fallback. Only lines carrying that context
    # are tolerated; any other use of the token still fails.
    matches=$(printf '%s\n' "$matches" | \
      grep -vi -e 'authority' -e 'NEXTOSCONTROLLERS' -e 'NEXTOS_CONTROLLERS' \
        -e 'NextOS applies' -e 'NextOS-own' -e 'NextOS shapes' || true)
  fi
  if [ -n "$matches" ]; then
    printf '%s\n' "$matches"
    echo "static-no-device-name-fallback: forbidden device/firmware token '$token' present" >&2
    status=1
  fi
done

# 0.10.0 (mission 5.8): the generic-fallback family of the quarantined
# 98e051f commit must never reappear in production source. Fixtures may
# contain exact GUIDs/names/mappings as EVIDENCE; production may not use
# them as heuristics, and no code may synthesize an `a:b0,b:b1,...` guess.
fallback_denylist=(
  'nx_add_generic_gamepad_mappings'
  'muOS Generic Xbox Fallback'
  'Generic Xbox Fallback'
)
for token in "${fallback_denylist[@]}"; do
  matches=$(grep -rIn --include='*.c' --include='*.h' --include='*.cpp' \
       -F -- "$token" "$scan_dir" "$header_dir" "$root/engine-glue" \
       2>/dev/null || true)
  if [ -n "$matches" ]; then
    printf '%s\n' "$matches"
    echo "static-no-device-name-fallback: quarantined generic fallback token '$token' present" >&2
    status=1
  fi
done
# A synthesized generic mapping literal in production C source (fixtures and
# tests legitimately carry exact official lines; src/ and glue never may).
matches=$(grep -rIn --include='*.c' --include='*.h' --include='*.cpp' \
     -E 'a:b[0-9]+,b:b[0-9]+,x:b[0-9]+,y:b[0-9]+' \
     "$scan_dir" "$root/engine-glue" 2>/dev/null || true)
if [ -n "$matches" ]; then
  printf '%s\n' "$matches"
  echo "static-no-device-name-fallback: synthesized mapping literal in production source" >&2
  status=1
fi

if [ "$status" -eq 0 ]; then
  echo "static-no-device-name-fallback: clean"
fi
exit "$status"
