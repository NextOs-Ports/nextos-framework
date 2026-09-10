# The Amazing Spider-Man 2 1.2.7d port notices

This package contains an independent clean-room compatibility loader. The
loader, launchers and game-specific preparation helpers are distributed under
GNU GPL version 3. The full license is preserved as
`licenses/GPL-3.0.txt`.

The package vendors NXExtract 1.1.2 under the MIT License, with one small
downstream discovery correction: a non-ZIP file explicitly supplied or
deliberately sniffed in the primary input directory remains a loose payload
candidate even when it has no extension. Acceptance still depends entirely on
the trusted recipe's exact size and SHA-256 checks. The complete MIT license is
preserved as `licenses/NXExtract-MIT.txt`.

The validated X5M route uses a narrowly scoped Box64/Box32 build under the
MIT License and an AArch64 `sdl2-compat` library under the zlib License. Their
exact upstream revisions, downstream patch and corresponding sources are
recorded in the adjacent source archive. The exact X5M profile is
`DYNAREC=1`, `BIGBLOCK=0`, `SAFEFLAGS=2`; experimental eager mode is excluded.
The package does not bundle SDL3, Mali, EGL/GLES or other firmware GPU
libraries.

The Amazing Spider-Man 2, its APK, `libtasm2.so`, `libtasm2-x86.so`, OBB
files, audio, trademarks and all other executable game data are proprietary
works of their respective rightsholders. They are not included in this ZIP and
are not covered by the loader's licenses. Users must provide files from their
own legitimate Android 1.2.7d copy. One real gameplay screenshot is included
solely as catalogue/release metadata.

This interoperability project is not affiliated with or endorsed by
Gameloft, Marvel or the other rightsholders.
