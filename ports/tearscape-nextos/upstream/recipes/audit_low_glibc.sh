#!/bin/bash
# Fail closed on every project-built ELF in the universal Tearscape package.
set -euo pipefail

if [ "$#" -lt 2 ]; then
	printf 'usage: %s TOOL_PREFIX ELF...\n' "$0" >&2
	exit 2
fi
TOOL_PREFIX=$1
shift
READELF="${TOOL_PREFIX}readelf"

# 0.2.12: the engine (first ELF) must carry dynamic udev joystick discovery.
# The 0.2.11 field failure was an engine built udev=no whose non-udev
# fallback could not see a combined controller. The udev path leaves its
# log literal in .rodata, so the shipped bytes prove it themselves. The
# second discovery layer (the nxinput 0.10.1 capability sweep) is a static
# function with no literal; it is proven by the framework pin chain
# (seam patch sha -> prepared source tree -> engine build receipt) and by
# the C6 stickless scenarios in the framework battery.
ENGINE_ELF=$1
# grep -c, never grep -q: under pipefail an early-exiting grep SIGPIPEs the
# strings producer and fails the pipeline exactly when the receipt EXISTS.
udev_receipts=$(strings -a "$ENGINE_ELF" | \
	grep -Fc 'Using udev for joystick device discovery' || true)
if [ "${udev_receipts:-0}" -eq 0 ]; then
	printf 'FAIL %s: dynamic udev joystick discovery is not compiled in\n' \
		"$ENGINE_ELF" >&2
	exit 1
fi
printf 'PASS %s joypad discovery receipt: udev-dynamic present\n' "$ENGINE_ELF"

version_le_230() {
	local version=${1#GLIBC_}
	local major=${version%%.*}
	local minor=${version#*.}
	minor=${minor%%.*}
	[ "$major" -lt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -le 30 ]; }
}

for elf in "$@"; do
	[ -f "$elf" ] && [ ! -L "$elf" ] || {
		printf 'FAIL: ELF is missing or not regular: %s\n' "$elf" >&2
		exit 1
	}
	machine=$("$READELF" -hW "$elf" | awk -F: '$1 ~ /^[[:space:]]*Machine/ {sub(/^[[:space:]]+/, "", $2); print $2}')
	[ "$machine" = AArch64 ] || {
		printf 'FAIL %s: machine=%s\n' "$elf" "$machine" >&2
		exit 1
	}
	max_glibc=$("$READELF" --version-info "$elf" 2>/dev/null | \
		grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1 || true)
	[ -n "$max_glibc" ] && version_le_230 "$max_glibc" || {
		printf 'FAIL %s: max glibc is %s\n' "$elf" "${max_glibc:-missing}" >&2
		exit 1
	}
	dynamic=$("$READELF" -dW "$elf")
	if printf '%s\n' "$dynamic" | grep -Eq '\((RPATH|RUNPATH|TEXTREL)\)'; then
		printf 'FAIL %s: RPATH, RUNPATH or TEXTREL present\n' "$elf" >&2
		exit 1
	fi
	"$READELF" -lW "$elf" | grep -q 'GNU_RELRO' || {
		printf 'FAIL %s: GNU_RELRO missing\n' "$elf" >&2
		exit 1
	}
	printf '%s\n' "$dynamic" | grep -Eq 'FLAGS(_1)?.*NOW' || {
		printf 'FAIL %s: immediate binding missing\n' "$elf" >&2
		exit 1
	}
	if "$READELF" -lW "$elf" | awk '$1 == "GNU_STACK" && $0 ~ /RWE/ {bad=1} END {exit !bad}'; then
		printf 'FAIL %s: executable stack\n' "$elf" >&2
		exit 1
	fi
	printf 'PASS %s machine=%s max-glibc=%s sha256=' "$elf" "$machine" "$max_glibc"
	sha256sum "$elf" | awk '{print $1}'
done
