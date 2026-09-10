# Forager Nuclear compatibility-port notices

The compatibility loader and packaging integration in this repository are
Copyright 2026 NextOS Project contributors and are distributed under GNU
General Public License version 2 only. The complete text is in `LICENSE`.

The loader is derived from JohnnyonFlame's gmloader-next at commit
`c2fca354df73761887c15f44a0b28ec823581cd5`. Local changes implement the narrow
Forager Android/GameMaker interoperability adapter. SDL2, EGL, GLES, OpenAL and
standard system libraries are supplied by the target firmware and are not
bundled.

NXExtract 1.2.6 (`nxextract.py`, `nxextract-ui`,
`nxextract-runtime-env.sh` and `run-extractor.sh`) is distributed under the MIT
license; see `licenses/NXExtract-MIT.txt`. Its exact hashes and the recipe hash
are recorded in `nxextract-version.txt`.

Forager, Forager Nuclear, their Android packages, native libraries, artwork,
music, sound effects, text, saves and all other owner data are proprietary works
of their respective rightsholders. They are separate from the compatibility
loader, are not covered by its license and are not included in this repository
or release. Users must provide files from their own lawful Android copy.

This independent interoperability project is not affiliated with or endorsed
by the game's publishers, developers, Google, YoYo Games or any other
rightsholder.
