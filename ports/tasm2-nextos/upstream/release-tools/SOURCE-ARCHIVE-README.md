# ASM2 port corresponding source

This archive contains the source, build recipes and license notices
corresponding to the public compatibility-loader package. It does not contain
the game APK, OBB files, game libraries, saves, logs, credentials or firmware
GPU libraries.

The ASM2 compatibility loader and its packaging helpers are distributed under
GNU GPL version 3. The complete license is in `licenses/GPL-3.0.txt`.

The i386 loader is linked with the GCC runtime statically. The GCC Runtime
Library Exception version 3.1 is preserved in
`licenses/GCC-Runtime-Library-Exception-3.1.txt`.

Bundled third-party source and exact revisions are recorded in
`SOURCE-PROVENANCE.json`: Box64/Box32 with the documented X5M patch,
`sdl2-compat`, and NXExtract with the small ASM2 discovery patch.

`SHA256SUMS` authenticates every archive member except itself. Timestamps,
ownership, ordering and modes are normalized by the source archive builder.
