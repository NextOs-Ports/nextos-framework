# Castle of Illusion — NextOS loader notices

The compatibility loader in this repository is part of `nextos_ports_android`,
Copyright 2026 NextOS contributors, and is distributed under GNU General Public
License version 3. The complete license text is in `LICENSE` and is copied into
public packages.

The Android/Bionic compatibility layer, the ELF64 custom loader and the
NativeActivity lifecycle implementation use patterns proven by the
GPL-3.0-licensed NextOS ports for Horizon Chase and Hitman GO. The
JNI surface, the FMOD Ex audio bridge, the GLES2 rendering path and the input
translation specific to the Sega "oz" engine were developed for this port.

The ETC1/ETC2 software codecs follow the public Khronos ETC1/ETC2/EAC format
specifications. SDL2, EGL, GLES, zlib and standard system libraries are
supplied by the target firmware and are not bundled.

NXExtract (the vendored BYO-data installer: `nxextract.py`, `nxextract-ui`,
`nxextract-runtime-env.sh`, `run-extractor.sh`) is distributed under the MIT
license — see `licenses/NXExtract-MIT.txt`; version and hashes are pinned in
`nxextract-version.txt`.

Castle of Illusion Starring Mickey Mouse, its Android APK, the `libViewer_GP`
engine library, FMOD Ex, the `main.154` OBB pack, levels, textures, artwork,
music, sound effects, saves and other game data are proprietary works of their
respective rightsholders. They are separate from this compatibility loader, are
not covered by the loader's license and are not distributed in the source tree
or the public package. Users must provide files from their own legitimate
Android installation.

FMOD Ex is a product of Firelight Technologies Pty Ltd. It is loaded from the
user's own game files and is not redistributed here.

This is an independent interoperability project. It is not affiliated with or
endorsed by Sega, Disney, Firelight Technologies or other rightsholders.
