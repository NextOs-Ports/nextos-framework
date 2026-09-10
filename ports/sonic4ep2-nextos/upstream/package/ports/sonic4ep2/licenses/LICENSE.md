# Sonic 4 Episode II Port - licenses and notices

This package is a BYO-data PortMaster/NextOS package. It includes only the
Linux compatibility loader, launcher scripts, metadata, a cue map, preview
images and first-run extraction helpers. It does not include the APK, OBB,
`libfox.so`, music, sound effects, textures, models, maps, videos or other game
data owned by SEGA or other rightsholders.

## Game data

"Sonic The Hedgehog 4: Episode II" is property of SEGA and its respective
rightsholders. The user must provide a legally obtained Android APK and
cache/OBB files. The launcher extracts the required data on first run.

This package does not grant any rights to redistribute SEGA game data.

## Port code

The compatibility loader and shims in this port are derived from the
`nextos_ports_android` so-loader framework and are released by the porter under
Apache-2.0 unless a source file states otherwise.

Relevant files include the `sonic4` binary source, JNI/EGL/Android shims, audio
bridge, launcher and extraction scripts.

## Runtime libraries

The `sonic4` binary is dynamically linked against libraries provided by the
target CFW/device. These libraries are not bundled in this zip:

- SDL2 (`libSDL2-2.0.so.0`) - zlib license.
- mpg123 (`libmpg123.so.0`) - LGPL-2.1-or-later style licensing.
- libogg/libvorbis/libvorbisfile - Xiph.Org BSD-style licenses.
- EGL/GLESv2, libc, libm, libpthread, libdl - provided by the target system.

Short notices for these dependencies are included in this directory.

## PortMaster helpers

`tools/progressor` is included to show progress during first-run extraction.
It comes from the PortMaster community extraction flow, reused from the
"TMNT: Shredder's Revenge" PortMaster method. Credits to the PortMaster
project and the original port authors.

`gptokeyb` is not bundled in this package; the launcher uses the copy provided
by the user's PortMaster/CFW installation when available.

## Package assets

The included `cover.png`, `screenshot.png` and `splash.png` are menu/package
assets for identifying the port. They are not a substitute for the game data
and do not grant rights to redistribute the original game.
