# Bundled LZ4 runtime

The managed-data preparation hook includes one AArch64 LZ4 shared library so
installation does not depend on an optional `liblz4` package in the target
firmware.

- upstream project: LZ4 by Yann Collet;
- binary distribution: Debian 10 (`buster`) arm64;
- Debian source package: `lz4`;
- Debian binary package: `liblz4-1` version `1.8.3-1+deb10u1`;
- original installed name: `/usr/lib/aarch64-linux-gnu/liblz4.so.1.8.3`;
- packaged name: `tools/liblz4.so.1`;
- SHA-256: `a65c53e2e7015b636e4f212449eff2016b99736cdf5798fe2cf3672818b88b8b`;
- ELF machine: AArch64;
- maximum required GNU libc symbol version: `GLIBC_2.17`.

The package build rejects a different hash, architecture, or GNU libc
requirement. The full BSD 2-Clause terms are distributed in
`licenses/LZ4-BSD-2-Clause.txt`.
