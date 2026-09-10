# Runtime provenance

## NXExtract

- Version: 1.2.0, recipe format 1.
- Synchronized byte-for-byte with the canonical multi-device source under
  `suportando_outros_devices/extrator-universal/`.
- `nxextract-runtime-env.sh` is also byte-identical to the canonical helper and
  keeps game-private libraries out of the extractor process.
- SHA-256: `55664066d2ff0e5b7b83b6285d6606cca74923e80183d2f2e176e6353b93abd5`
  (`nxextract.py`) and
  `332919a9960d4317563b647f9932d1a4367da147a425fe2f78eafd706f01563f`
  (`nxextract-runtime-env.sh`).
- License: MIT (`licenses/NXExtract-MIT.txt`).

## LZ4

- Runtime file: `tools/liblz4.so.1`.
- SHA-256:
  `a65c53e2e7015b636e4f212449eff2016b99736cdf5798fe2cf3672818b88b8b`.
- Size: 108824 bytes; AArch64; maximum glibc requirement 2.17.
- Origin: Debian 10 LZ4 1.8.3 runtime package.
- License: BSD-2-Clause (`licenses/LZ4-BSD-2-Clause.txt`).

## UnityPy and attrs

- UnityPy 1.22.5, pinned because it supports Python 3.7 used by the tested
  ArkOS device.
- Local changes remove unrelated optional imports, use a local-only filesystem
  adapter and call the bundled LZ4 through `ctypes`.
- attrs 23.2.0 is vendored for UnityPy.
- Licences: MIT (`licenses/UnityPy-MIT.txt`, `licenses/attrs-MIT.txt`).

## PF2 v1.09.3 transformation masks

The developer-only generator is `tools/dev/generate_pairip_masks.py`. It XORs
four owner-captured Android plaintext windows with their matching encrypted
ranges from the exact v1.09.3 ARM64 libraries. The release distributes only the
resulting masks:

```text
2a5100d0964e334a700df9f3f015298ba0fb5709faea914a97a3c0c330f5b3af  libil2cpp.text.xormask
1fccf72b18c607fe7d1e1a06c01d4fb7c7df92a9239a6f018c1fe35e5924f25b  libil2cpp.data.xormask
34c46bca967ef8c667805f045b0f382a382254180716f95e0dc762e7b09ac7a0  libunity.text.xormask
170f471d673b916cf6d3c6bfbbddf7a50edaf41731813898991ff27d8ab8ac5c  libunity.data.xormask
```

Before applying a mask, `prepare_pf2_data.py` checks the full source-library
size and SHA-256. It then validates the reconstructed window's exact size and
SHA-256. Unsupported APK revisions stop without committing data.
