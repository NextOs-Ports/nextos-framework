# Bomb Chicken compatibility-port notices

The compatibility loader and packaging integration in this directory are part
of `nextos_ports_android`, Copyright 2026 NextOS Project contributors, and are
distributed under GNU General Public License version 3 only. The complete text
is in `LICENSE`.

The loader follows interoperability techniques developed in the free-software
Android native-porting community. Required acknowledgements include mtojek's
Apache-2.0 Android native loader work, initdream's Crazy Taxi work and
Producdevity's MIT-licensed Call of Duty: Black Ops Zombies work. Their
copyrights and licenses remain their own; this notice does not relicense those
upstream projects.

The Bomb Chicken adapter preserves the game's observed Unity
2022.3.39f1/IL2CPP lifecycle. Its v44-specific JNI, pause, progress, input and
presentation corrections are enabled only by the generated port contract for
the exact accepted owner payload. The ETC2 software decoder follows the public
Khronos ETC2/EAC format specification. SDL2, EGL, GLES, zlib and standard
system libraries are supplied by the target firmware and are not bundled.

NXExtract 1.2.6 (`nxextract.py`, `nxextract-ui`,
`nxextract-runtime-env.sh`, `run-extractor.sh`) is distributed under the MIT
license; see `licenses/NXExtract-MIT.txt`. Its exact unmodified hashes and the
recipe hash are recorded in `nxextract-version.txt`.

Bomb Chicken, its Android APK, Unity/IL2CPP libraries, scenes, textures,
artwork, music, sound effects, saves and all other owner data are proprietary
works of Nitrome or their respective rightsholders. They are separate from the
compatibility loader, are not covered by its license and are not present in the
source tree or public package. Users must provide files from their own lawful
Android copy.

This independent interoperability project is not affiliated with or endorsed
by Nitrome, Unity Technologies, Google or any other rightsholder.
