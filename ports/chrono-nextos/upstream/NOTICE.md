# Chrono Trigger — NextOS loader notices

The compatibility loader in this directory is part of `nextos_ports_android`,
Copyright 2026 NextOS contributors, and is distributed under GNU General Public
License version 3. The complete license text is in the repository root and is
copied into public packages as `LICENSE`.

The common compatibility components (`nxbootstrap`, `nxcompat`, `nxloader`,
`nxgl`, `nxinput` and `nxaudio`) and the Chrono-specific AArch64/Bionic adapter
are GPL-3.0. The fake JNI environment, Cocos2d-x 3.14.1 lifecycle, OpenSL ES
audio bridge, FreeType text-bitmap path and native `GameControllerAdapter`
interoperability remain specific to this port.

SDL2, EGL/GLES2, FreeType and the standard system libraries are supplied by the
target firmware and are not bundled. The public executable is built against
GLIBC 2.27 so it runs on the low-glibc firmwares of the supported handhelds.

The bundled UI font is **Noto Sans Regular**, Copyright 2022 The Noto Project
Authors, under the SIL Open Font License 1.1 (`fonts/OFL.txt`). It replaces the
Android system font (Roboto) that the game expects but does not ship; no font
from an Android device is redistributed. The same font is embedded in the
executable as the guaranteed fallback; `CHRONO_FONT` or the `fonts/` directory
take precedence, and a firmware sans font is only used if all of those fail.

NXExtract (the vendored BYO-data installer under `nxextract/`) is distributed
under the MIT license — see `licenses/NXExtract-MIT.txt`; version 1.2.6 and the
hash of every runtime file are pinned in `nxextract-version.txt`.

Chrono Trigger, its Android APK, `libchrono.so`, `libc++_shared.so`,
`libencrypt.so`, the encrypted asset archives (`resources.bin`, `00N.dat`),
shaders, artwork, music, sound effects, saves and other game data are
proprietary works of their respective rightsholders. They are separate from
this compatibility loader, are not covered by the loader's license and are not
distributed in the source tree or in the public package. Users must provide
those files from their own legitimate Android purchase.

This is an independent interoperability project. It is not affiliated with or
endorsed by Square Enix, Cocos/Chukong Technologies, Google or any other
rightsholder.
