# Third-party notices

This port's host runtime is GPL-3.0-only. The game itself is never
distributed here: Nameless Cat, its engine libraries, its art and its audio
are the property of Kotoba Games and are supplied by the player from a
legally owned copy.

## NXExtract

The installer bundled under `nxextract/` is NXExtract, MIT licensed. See
`licenses/NXExtract-MIT.txt`.

## NXSplash

The canonical NXSplash helper is MIT licensed. See `NXSPLASH-LICENSE`.

## SDL2 (private, seam-carrying)

`lib/libSDL2-2.0.so.0` is SDL 2.32.10 (zlib license, see
`licenses/SDL2-zlib.txt`) built from the pinned upstream release plus the
nxinput C6 seam of the NextOS framework (GPL-3.0-only, sources vendored under
`vendor/nxinput/` with their SHA-256 pins in `vendor/nxinput/PINS.json`).
ALSA, PulseAudio, libudev, libdrm/gbm and EGL/GLES are loaded dynamically at
run time from the device firmware; none of them is distributed here.
