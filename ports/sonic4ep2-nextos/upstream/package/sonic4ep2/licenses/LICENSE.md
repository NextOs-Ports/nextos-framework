# Sonic 4 Episode II V6 - licenses and notices

This is a BYO-data PortMaster/NextOS compatibility package. It includes the
Linux compatibility loader, launcher, metadata, native controller handling,
cue map, preview images, setup helper and redistributable audio runtime
libraries. It does not include the APK, OBB, `libfox.so`, music, sound effects,
textures, models, maps, videos or other runtime game data owned by SEGA or other
rightsholders.

## Game data

Sonic The Hedgehog 4: Episode II is property of SEGA and its respective
rightsholders. The user must provide a legally obtained Android v3.0.0-109
ARM64 copy. This package grants no right to redistribute those game files.

## Port code

The compatibility loader and shims are derived from the
`nextos_ports_android` so-loader framework and are released under the
GNU GPL-3.0 unless an individual source file states otherwise.
See `GPL-3.0.txt`. Releases built before 2026-07-29 carried an Apache-2.0
notice for this code; Apache-2.0 is GPL-compatible and those artifacts remain
valid.

## Runtime libraries

The package bundles the following AArch64 fallback libraries so the port can
run on minimal firmware images that do not provide them:

- mpg123 (`libmpg123.so.0`), LGPL-2.1-or-later;
- libogg (`libogg.so.0`), Xiph.Org BSD-style license;
- libvorbis and libvorbisfile, Xiph.Org BSD-style license.

The launcher places firmware libraries before these fallback copies in its
search path. See `LGPL-2.1-or-later.txt`, `mpg123-LGPL-NOTICE.txt` and
`Xiph-Ogg-Vorbis-BSD.txt`.

SDL2, EGL/GLESv2, libc, libm, libpthread and libdl are supplied by the target
firmware and are not bundled. See `SDL2-zlib.txt` for the SDL2 notice.

## Package assets

The included `box.png`, `cover.png`, `screenshot.png` and `splash.png` are
frontend assets for identifying the port. They are not substitutes for the
game data and do not grant rights to redistribute the original game.
