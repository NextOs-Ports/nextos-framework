# Hitman GO — notices

The compatibility loader is Copyright 2026 NextOS contributors and is
distributed under GNU General Public License version 3. The complete license
is included as `LICENSE`.

The Android/Bionic loader, Unity lifecycle adapter, JNI surface, input bridge,
graphics compatibility and audio bridge in this repository are original
interoperability work for the legally obtained Android release. Hitman-specific
offsets and hooks stay inside this adapter and are not framework defaults.

The public package pins these independently licensed framework artifacts:

- nxbootstrap 0.6.14 and nxgenerator 0.2.8 (GPL-3.0-only);
- NXExtract 1.2.9 and NXSplash 0.1.2 (MIT);
- nxrelease 0.2.12 (GPL-3.0-only).

Exact immutable tags and archive SHA-256 values are recorded in
`FRAMEWORK-PIN.json`; generated-file identities are recorded in
`GENERATION.json`. NXExtract and NXSplash license texts are under `licenses/`.

SDL2, EGL, GLES, zlib, libc and firmware libraries are supplied by the target
system and retain their own licenses.

Hitman GO, its APK, Unity/IL2CPP/Firebase libraries, scenes, textures, artwork,
music, sound effects, saves and all other game data are proprietary works of
their respective rightsholders. They are not included, downloaded or licensed
by this project. Users must supply their own legitimate Android copy.

This is an independent interoperability project. It is not affiliated with or
endorsed by Square Enix Montreal, Square Enix, Unity Technologies, Google or
other rightsholders.
