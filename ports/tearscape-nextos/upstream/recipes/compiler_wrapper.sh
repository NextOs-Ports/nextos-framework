#!/bin/bash
# Run Arm GNU 10.3 against the pinned Debian Buster AArch64 glibc sysroot.
set -euo pipefail

TOOLCHAIN_DIR=${TEARSCAPE_TOOLCHAIN_DIR:-/toolchain}
BUSTER_SYSROOT=${TEARSCAPE_BUSTER_SYSROOT:-/buster}
GCC_VERSION=10.3.1
TRIPLE=aarch64-none-linux-gnu

invoked_as=$(basename -- "$0")
case "$invoked_as" in
	*g++*|*c++*)
		driver="$TOOLCHAIN_DIR/bin/$TRIPLE-g++"
		cxx_root="$TOOLCHAIN_DIR/$TRIPLE/include/c++/$GCC_VERSION"
		language_includes=(
			-isystem "$cxx_root"
			-isystem "$cxx_root/$TRIPLE"
			-isystem "$cxx_root/backward"
			-include /port/recipes/gcc10_buster_compat.h
		)
		;;
	*gcc*|*cc*)
		driver="$TOOLCHAIN_DIR/bin/$TRIPLE-gcc"
		language_includes=()
		;;
	*)
		printf 'unsupported compiler wrapper name: %s\n' "$invoked_as" >&2
		exit 1
		;;
esac

[ -x "$driver" ] && [ -f "$BUSTER_SYSROOT/usr/include/features.h" ] || {
	printf 'low-glibc compiler inputs are incomplete\n' >&2
	exit 1
}

exec "$driver" \
	--sysroot="$BUSTER_SYSROOT" \
	-nostdinc \
	"${language_includes[@]}" \
	-isystem "$TOOLCHAIN_DIR/lib/gcc/$TRIPLE/$GCC_VERSION/include" \
	-isystem "$BUSTER_SYSROOT/usr/include" \
	-isystem "$TOOLCHAIN_DIR/lib/gcc/$TRIPLE/$GCC_VERSION/include-fixed" \
	"$@"
