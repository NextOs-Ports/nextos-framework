# Swordigo (NextOS so-loader)

## Credits
- TouchFoo / Ville Mäkynen — original game (obtain legally; BYO-data)
- TheFloW / Rinnegatamante — original Vita so-loader approach
- NaGaa95 — swordigo_nx (Switch wrapper for APK 1.4.12; architecture reference)
- mtojek — Linux ARM64 so-loader base (Apache-2.0)
- NextOS ports framework contributors

## Third-party
- mpg123 1.31.3 — bundled under LGPL-2.1-or-later; its license is included in
  `licenses/mpg123-LGPL-2.1-or-later.txt`
- OpenAL Soft, SDL2, GLES/EGL and zlib — supplied by the target firmware or
  PortMaster runtime
- Boost, Lua, protobuf, OpenAL Soft (Apportable fork) — shipped inside the APK
  (see APK assets/README); not redistributed by this repository

This repository contains only loader/shim source. No game assets or binaries.
