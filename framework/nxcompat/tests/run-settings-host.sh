#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Hermetic host gate for V3-SETTINGS-01: language V2 resolver + settings
# parser.  Pure C, no device, no SDL, no filesystem in the code under test.
set -euo pipefail

nxcompat_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
work_root=$(mktemp -d /tmp/nxcompat-settings-host.XXXXXX)

cleanup() {
  local status=$?
  trap - EXIT
  if [[ $work_root == /tmp/nxcompat-settings-host.* && -d $work_root ]]; then
    find "$work_root" -depth -delete
  fi
  exit "$status"
}
trap cleanup EXIT

compilers=()
for candidate in gcc clang; do
  if command -v "$candidate" >/dev/null 2>&1; then
    compilers+=("$candidate")
  fi
done
if [[ ${#compilers[@]} -eq 0 ]]; then
  printf 'no C compiler found (need gcc or clang)\n' >&2
  exit 1
fi

corpus_count=0

for cc in "${compilers[@]}"; do
  for name in language_v2 settings video; do
    "$cc" -std=c99 -Wall -Wextra -Werror -O1 \
      -I "$nxcompat_root/include" \
      "$nxcompat_root/src/nxcompat_${name}.c" \
      "$nxcompat_root/tests/test_${name}.c" \
      -o "$work_root/test-${name}-${cc}"
    "$work_root/test-${name}-${cc}"
  done

  # V5 FV3 / 7A.2 (nxcompat 0.5.1): the owner election/precedence/readback
  # gate links the SAME nxcompat_video translation unit as the geometry gate.
  "$cc" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$nxcompat_root/include" \
    "$nxcompat_root/src/nxcompat_video.c" \
    "$nxcompat_root/tests/test_video_owner.c" \
    -o "$work_root/test-video-owner-${cc}"
  "$work_root/test-video-owner-${cc}"

  # V3-HARDENING-01: deterministic adversarial corpus replay through the
  # real parser (ok-* accepted, bad-* rejected back to the safe defaults).
  "$cc" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$nxcompat_root/include" \
    "$nxcompat_root/src/nxcompat_settings.c" \
    "$nxcompat_root/tests/corpus_replay_settings.c" \
    -o "$work_root/corpus-replay-settings-${cc}"
  corpus_count=$(timeout 120 "$work_root/corpus-replay-settings-${cc}" \
    "$nxcompat_root/tests/corpus-settings")
done

printf 'nxcompat_settings_host=PASS compilers=%s tests=4 ' \
  "$(IFS=,; printf '%s' "${compilers[*]}")"
printf 'pure_functions=1 env_mutation=0 filesystem_in_parser=0 corpus=%s\n' \
  "$corpus_count"
