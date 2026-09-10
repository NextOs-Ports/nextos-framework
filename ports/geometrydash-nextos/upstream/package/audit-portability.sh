#!/usr/bin/env bash
# Audit a public PortMaster staging tree. Host-side tool; never run game data.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: audit-portability.sh [--max-glibc VERSION] [--allow FILE] STAGE

VERSION may make the public gate stricter, but can never exceed 2.30.

Allowlist format (only for an adaptive backend assignment or supervised child):
  adaptive-driver|relative/path.sh|reason with evidence
  supervised-child|relative/path.sh|PID, trap and wait contract
EOF
  exit 64
}

public_max_glibc=2.30
max_glibc=$public_max_glibc
allow_file=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --max-glibc)
      [ "$#" -ge 2 ] || usage
      max_glibc=$2
      shift 2
      ;;
    --allow)
      [ "$#" -ge 2 ] || usage
      allow_file=$2
      shift 2
      ;;
    --help|-h) usage ;;
    --*) usage ;;
    *) break ;;
  esac
done
[ "$#" -eq 1 ] || usage

stage=$1
[ -d "$stage" ] && [ ! -L "$stage" ] || {
  printf 'AUDIT FAIL: stage is missing, not a directory, or is a symlink: %s\n' "$stage" >&2
  exit 1
}
stage=$(CDPATH= cd -- "$stage" && pwd -P)

if [[ ! "$max_glibc" =~ ^[0-9]+([.][0-9]+)+$ ]]; then
  printf 'AUDIT FAIL: invalid GLIBC ceiling: %s\n' "$max_glibc" >&2
  exit 1
fi
if [ -n "$allow_file" ]; then
  [ -f "$allow_file" ] || {
    printf 'AUDIT FAIL: allowlist not found: %s\n' "$allow_file" >&2
    exit 1
  }
  allow_file=$(CDPATH= cd -- "$(dirname -- "$allow_file")" && pwd -P)/$(basename -- "$allow_file")
fi

failures=0
elf_count=0
max_seen=none

fail() {
  printf 'AUDIT FAIL [%s]: %s\n' "$1" "$2" >&2
  failures=$((failures + 1))
}

is_allowed() {
  local rule=$1 relative=$2
  [ -n "$allow_file" ] || return 1
  awk -F '|' -v rule="$rule" -v path="$relative" '
    $1 == rule && $2 == path && length($3) > 0 { found=1 }
    END { exit found ? 0 : 1 }
  ' "$allow_file"
}

version_gt() {
  local left=$1 right=$2 greatest
  greatest=$(printf '%s\n%s\n' "$left" "$right" | sort -V | tail -n 1)
  [ "$greatest" = "$left" ] && [ "$left" != "$right" ]
}

if version_gt "$max_glibc" "$public_max_glibc"; then
  printf 'AUDIT FAIL: requested GLIBC ceiling %s exceeds the public ceiling %s\n' \
    "$max_glibc" "$public_max_glibc" >&2
  exit 1
fi

elf_required_glibc() {
  local file=$1
  LC_ALL=C readelf --version-info --wide "$file" 2>/dev/null |
    awk '
      /Version needs section/ { needs=1; next }
      needs && /Version definition section/ { needs=0 }
      needs { print }
    ' |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)+' |
    sed 's/^GLIBC_//' |
    sort -Vu || true
}

command -v readelf >/dev/null 2>&1 || {
  printf '%s\n' 'AUDIT FAIL: readelf is required' >&2
  exit 1
}

if [ -f "$stage/SHA256SUMS" ]; then
  command -v sha256sum >/dev/null 2>&1 || {
    printf '%s\n' 'AUDIT FAIL: SHA256SUMS exists but sha256sum is unavailable' >&2
    exit 1
  }
  while IFS= read -r manifest_line || [ -n "$manifest_line" ]; do
    case "$manifest_line" in
      ''|'#'*) continue ;;
    esac
    if [[ ! "$manifest_line" =~ ^[0-9a-fA-F]{64}[[:space:]][\ *] ]]; then
      fail manifest "SHA256SUMS has a non-GNU or malformed line"
      continue
    fi
    manifest_path=${manifest_line:66}
    manifest_path=${manifest_path#\*}
    case "$manifest_path" in
      ''|/*|../*|*/../*|*/..)
        fail manifest "SHA256SUMS has an unsafe path: $manifest_path"
        ;;
    esac
  done <"$stage/SHA256SUMS"
  if ! (cd -- "$stage" && sha256sum --check --strict SHA256SUMS >/dev/null); then
    fail manifest 'SHA256SUMS verification failed'
  fi
fi

while IFS= read -r -d '' path; do
  relative=${path#"$stage"/}

  if [ -L "$path" ]; then
    fail symlink "$relative"
    continue
  fi
  [ -f "$path" ] || continue

  lower=${relative,,}
  case "$lower" in
    *.apk|*.apkm|*.apks|*.xapk|*.obb|*.dex|*.pdb|*.log|*.pyc|*/__pycache__/*|*/cache/*|core|core.*|*/core|*/core.*|*global-metadata.dat|*sharedassets*.assets)
      fail forbidden-data "$relative"
      ;;
  esac

  if LC_ALL=C grep -Iq . "$path" 2>/dev/null; then
    if LC_ALL=C grep -En '/home/[A-Za-z0-9._-]+|/Users/[A-Za-z0-9._-]+' "$path" >/dev/null 2>&1; then
      fail private-path "$relative"
    fi
    if LC_ALL=C grep -En '(^|[^0-9])([0-9]{1,3}[.]){3}[0-9]{1,3}([^0-9]|$)' "$path" >/dev/null 2>&1; then
      fail ipv4-literal "$relative"
    fi
  fi

  case "$lower" in
    *.sh)
      if IFS= read -r first <"$path" && [[ "$first" == *bash* ]]; then
        bash -n "$path" || fail shell-syntax "$relative"
      else
        sh -n "$path" || fail shell-syntax "$relative"
      fi

      if LC_ALL=C grep -En '^[[:space:]]*([^#].*[[:space:];])?(setsid|nohup)([[:space:]]|$)' "$path" >/dev/null 2>&1; then
        fail detached-process "$relative"
      fi
      if LC_ALL=C grep -Eini '^[[:space:]]*[^#]*(systemctl([[:space:]]|$)|(pkill|killall).*(emustation|emulationstation))' "$path" >/dev/null 2>&1; then
        fail frontend-management "$relative"
      fi
      if LC_ALL=C grep -En '^[[:space:]]*[^#]*((export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER|ALSOFT_DRIVERS)=' "$path" >/dev/null 2>&1; then
        if ! is_allowed adaptive-driver "$relative"; then
          fail forced-driver "$relative (use a documented adaptive-driver allow entry only after a real failed probe)"
        fi
      fi
      if LC_ALL=C grep -En '^[[:space:]]*[^#]*[^&]&[[:space:]]*(#.*)?$' "$path" >/dev/null 2>&1; then
        if ! is_allowed supervised-child "$relative"; then
          fail background-child "$relative (requires exact PID, trap, wait and a supervised-child allow entry)"
        fi
      fi
      ;;
  esac

  magic=$(LC_ALL=C od -An -t x1 -N 4 "$path" 2>/dev/null | tr -d ' \n')
  [ "$magic" = 7f454c46 ] || continue
  elf_count=$((elf_count + 1))
  if ! readelf -h "$path" >/dev/null 2>&1; then
    fail malformed-elf "$relative"
    continue
  fi

  elf_max=none
  while IFS= read -r required; do
    [ -n "$required" ] || continue
    if [ "$elf_max" = none ] || version_gt "$required" "$elf_max"; then
      elf_max=$required
    fi
    if [ "$max_seen" = none ] || version_gt "$required" "$max_seen"; then
      max_seen=$required
    fi
    if version_gt "$required" "$max_glibc"; then
      fail glibc "$relative needs GLIBC_$required (> GLIBC_$max_glibc)"
    fi
  done < <(elf_required_glibc "$path")
  elf_class=$(LC_ALL=C readelf -h "$path" | awk -F: '$1 ~ /Class/ {gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
  elf_machine=$(LC_ALL=C readelf -h "$path" | awk -F: '$1 ~ /Machine/ {gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
  elf_interpreter=$(LC_ALL=C readelf -lW "$path" 2>/dev/null |
    awk '/Requesting program interpreter/ {
      sub(/^.*Requesting program interpreter: /, "")
      sub(/].*$/, "")
      print
      exit
    }')
  [ -n "$elf_interpreter" ] || elf_interpreter=none
  case "$elf_interpreter" in
    none|/*) ;;
    *) fail interpreter "$relative has a non-absolute PT_INTERP: $elf_interpreter" ;;
  esac
  case "$elf_interpreter" in
    /home/*|/Users/*) fail private-path "$relative PT_INTERP=$elf_interpreter" ;;
  esac
  elf_needed=$(LC_ALL=C readelf -dW "$path" 2>/dev/null |
    awk -F '[][]' '/[(]NEEDED[)]/ {print $2}' |
    paste -sd, -)
  [ -n "$elf_needed" ] || elf_needed=none
  if command -v sha256sum >/dev/null 2>&1; then
    elf_sha=$(sha256sum "$path" | awk '{print $1}')
  else
    elf_sha=unavailable
  fi
  printf 'ELF: %s | class=%s | machine=%s | interp=%s | needed=%s | glibc=%s | sha256=%s\n' \
    "$relative" "${elf_class:-unknown}" "${elf_machine:-unknown}" \
    "$elf_interpreter" "$elf_needed" "$elf_max" "$elf_sha"
done < <(find "$stage" -mindepth 1 -print0 | sort -z)

if [ "$failures" -ne 0 ]; then
  printf 'AUDIT RESULT: FAIL failures=%d elfs=%d max_glibc=%s\n' "$failures" "$elf_count" "$max_seen" >&2
  exit 1
fi

printf 'AUDIT RESULT: PASS files=%s elfs=%d max_glibc=%s ceiling=%s\n' \
  "$(find "$stage" -type f | wc -l)" "$elf_count" "$max_seen" "$max_glibc"
