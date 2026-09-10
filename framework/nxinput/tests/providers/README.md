# Provider pins (V5)

`provider-manifest-v5.json` pins the bytes of every SDL provider this line has
seen to the ordinal table an independent tool reproduced for it. The compiled
table in `src/nxinput_provider.c` must agree with it (`tests/v5/test_v5_provider_pins.py`).

`patches/` holds the Batocera/RetroArch input patches copied verbatim from the
pinned Knulli distribution commit (GPL-2.0 buildroot patches over zlib SDL);
they are the transcription source of the `sdl2-batocera-ascending` domain.

Large artefacts (firmware images, DSOs) stay outside Git; the manifest carries
their SHA-256 and origin.
