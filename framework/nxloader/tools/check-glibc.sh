#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Public GLIBC ceiling gate.
#
# M17-003: the ceiling is not hardcoded here any more.  It is read from
# framework/nxabi/policy-v1.json, the single source of truth every checker in
# the repository shares.  NXLOADER_GLIBC_MAX still overrides it for a local
# experiment, never for a release.
#
# M17-003 also requires GLIBC_PRIVATE to fail: the previous numeric-only scan
# silently ignored it.
set -euo pipefail
export LC_ALL=C

TOOLS_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
POLICY=${NXABI_POLICY:-$TOOLS_ROOT/../../nxabi/policy-v1.json}

policy_ceiling() {
  [[ -r $POLICY ]] || return 1
  python3 -c '
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as stream:
    data = json.load(stream)
value = data.get("ceilings", {}).get("glibc_max")
if not isinstance(value, str):
    raise SystemExit(1)
print(value)
' "$POLICY" 2>/dev/null
}

if [[ -n ${NXLOADER_GLIBC_MAX:-} ]]; then
  limit=$NXLOADER_GLIBC_MAX
elif limit=$(policy_ceiling); then
  :
else
  echo "check-glibc: cannot read the ceiling from $POLICY" >&2
  exit 2
fi

failed=0
checked=0

if [[ "$#" -eq 0 ]]; then
  echo "usage: $0 ELF_OR_DIRECTORY [...]" >&2
  exit 2
fi

version_is_above() {
  awk -v found="$1" -v limit="$2" 'BEGIN {
    split(found, f, "."); split(limit, l, ".");
    fn = (f[1] + 0) * 1000000 + (f[2] + 0) * 1000 + (f[3] + 0);
    ln = (l[1] + 0) * 1000000 + (l[2] + 0) * 1000 + (l[3] + 0);
    exit !(fn > ln)
  }'
}

check_one() {
  local file="$1" info versions version
  if ! readelf -h -- "$file" >/dev/null 2>&1; then
    return 0
  fi
  checked=$((checked + 1))
  info="$(readelf --version-info -- "$file" 2>/dev/null || true)"
  if grep -q 'GLIBC_PRIVATE' <<<"$info"; then
    echo "FAIL: $file requires GLIBC_PRIVATE" >&2
    failed=1
  fi
  versions="$(sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' <<<"$info" | sort -Vu)"
  while IFS= read -r version; do
    [[ -n "$version" ]] || continue
    if version_is_above "$version" "$limit"; then
      echo "FAIL: $file requires GLIBC_$version (maximum GLIBC_$limit)" >&2
      failed=1
    fi
  done <<<"$versions"
}

for target in "$@"; do
  if [[ -d "$target" ]]; then
    while IFS= read -r -d '' file; do
      check_one "$file"
    done < <(find "$target" -type f -print0)
  elif [[ -f "$target" ]]; then
    check_one "$target"
  else
    echo "missing path: $target" >&2
    failed=1
  fi
done

if [[ "$checked" -eq 0 ]]; then
  echo "no ELF files found" >&2
  exit 2
fi
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
echo "glibc gate passed: $checked ELF(s), maximum GLIBC_$limit"
