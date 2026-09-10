# Terraria Android compatibility loader — notices

The compatibility loader in this repository is part of
`nextos_ports_android`, Copyright 2026 NextOS contributors, and is distributed
under GNU GPL version 3. The complete text is in `LICENSE` and
`licenses/GPL-3.0.txt`.

The reusable Android compatibility-loader framework includes work derived
from:

- `syberia_arm64` and `lswtcs_arm64` by mtojek, Apache License 2.0;
- the `max_arm64` lineage by Jaakko Lukkari, fgsfds and Andy Nguyen;
- interoperability work from initdream's Crazy Taxi loader;
- `cod-boz-port` by Producdevity, MIT License.

`src/hashmap.h` is `sheredom/hashmap.h`, released under the Unlicense.
Corresponding license texts are preserved in `licenses/`.

The bionic pthread bridge and controller-keyboard interaction/layout are
adapted from the GPL-3.0-licensed Prizefighters 2 NextOS port. The bionic
signal-set and multi-firmware lifecycle strategy follows the GPL-3.0-licensed
Horizon Chase NextOS port. Terraria-specific managed-name integration,
row-aware navigation and the Terraria-themed rendering palette were developed
for this port. Both working references are maintained by NextOS contributors.

NXExtract 1.2.0 is a separate MIT-licensed component whose complete source is
vendored under `third_party/NXExtract/`; its license is also copied to
`licenses/NXExtract-MIT.txt`.

SDL2, EGL, GLES and the standard system libraries are supplied by the target
firmware and are not bundled.

Terraria, its Android APK, Unity/IL2CPP native libraries, scenes, textures,
artwork, music, sound effects, saves and other game data are proprietary works
of their respective rightsholders. They are not covered by the source-code
licenses and are not distributed in this repository or its public package.
Users must provide data from their own legitimate Android installation.

This is an independent interoperability project. It is not affiliated with or
endorsed by Re-Logic, 505 Games, Unity Technologies or other rightsholders.
