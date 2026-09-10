# Changelog

## 1.1.1 — 2026-07-31

- Fixed the ROCKNIX/RG-DS pre-main black screen: `glDrawTexfOES` is now an
  optional runtime extension instead of a mandatory loader symbol.
- Exact 4:3 displays such as 640x480 now use the whole panel by default;
  `SUMMERTIME_DISPLAY_MODE=fit` restores the original 16:9 letterbox.
- Reduced pointer speed by 20% only on 640x480-class panels. The multiplier is
  configurable through `SUMMERTIME_CURSOR_SCALE`.
- Audio now applies the physically validated ROCKNIX server-to-ALSA policy:
  an inherited PulseAudio-only selection uses `aplay` directly when ALSA is
  available, while a failed automatic `pacat` sink recovers once through ALSA.
  Explicit diagnostic choices remain available through
  `SUMMERTIME_AUDIO_DRIVER`.

## 1.1.0 — 2026-07-30

- Unlocked ArkOS/R36-class firmwares (glibc 2.28): provided bionic-safe
  wrappers for the `sigset_t`, `mbstate_t` and `stat` family plus
  `strlcpy`/`wcslcpy`, fixing a boot crash caused by ABI size mismatches
  that newer glibc masked.
- Key-based D-pads (BTN_DPAD_*, e.g. RG351/GO-Super) now move the pointer
  exactly like hat/stick D-pads.
- The left face button now sends keyboard Enter, submitting Ren'Py text
  screens such as the character name prompt.
- NXExtract 1.1.2: the per-launch marker check no longer walks the whole
  payload tree, making every boot instant on SD-card devices.
- Fixed vertical click offset on 4:3 screens: pointer clicks are now mapped
  through the letterboxed 16:9 rectangle Ren'Py actually draws, so the click
  always lands on the arrow tip.
- ALSA fallback for the audio bridge: firmwares without PulseAudio
  (ArkOS/R36) now play through `aplay` automatically.
- Native shortcut buttons (editable tap macros in `summertimesaga.gptk`):
  Y opens the phone, L1 the backpack, R1 the town map, L2 the save screen,
  R2 quick-saves. The right face button is now a universal Android BACK.
- Added an `ultra-lowmem` GPU profile for 640 MB-class devices.
- Select+Start clean exit now also recognizes the RG351/GO-Super family
  function buttons (`BTN_TRIGGER_HAPPY`), validated on device.

- Replaced the blocky cursor with one classic anti-aliased arrow (black
  outline, white core, soft drop shadow, red on hover), pre-rasterized at
  build time. Mali-450 keeps the OSD2 hardware layer; every other GPU now
  draws the same arrow as a premultiplied textured quad before each swap,
  with full GL state save/restore.
- Moved the GPU/texture profile out of the launcher: the loader now measures
  the real GLES context (ES3 with ES2 fallback, desktop GL rejected) and
  physical memory, then fills only unset variables. Strong GLES3 devices keep
  full-size textures; 1 GB GLES2 devices keep the validated Mali-450
  ETC1/16-bit path. Every variable remains an engineering override.
- Published the hover state to the GL arrow as well, so the red hover
  feedback works on every device, not only on the Mali-450 hardware cursor.

## 1.0.1 — 2026-07-30

- Fixed the first-run black screen on the tested NextOS S905X5M route by
  keeping NXExtract on the firmware's native SDL2/KMSDRM stack.
- Prevented inherited game or compatibility-library paths and SDL selections
  from leaking into the X5M extractor process.
- Kept the PortMaster-facing wrapper compatible with control files that
  initialize firmware variables lazily instead of aborting under `nounset`.
- Applied the same process-scope boundary to future Preview updates and added
  a package contract test that rejects regressions or frontend service
  manipulation.

## 1.0.0 — 2026-07-30

- Added a universal AArch64 package for NextOS, NextOS Elite, ArkOS/R36S and
  compatible handheld Linux systems.
- Added separate current-NextOS and GLIBC 2.30-compatible native loaders.
- Integrated NXExtract for content-driven APK/APKS/APKM/XAPK installation.
- Added transactional `--force-source` updates without destroying the
  previously validated payload.
- Added structural support for compatible future Preview APKs without
  hard-coding a filename or one APK hash.
- Added dynamic `x-` module de-prefixing and deterministic asset indexing.
- Preserved Mali-450 ETC1 memory fixes, SDL audio, evdev controls and OSD2
  hardware cursor, with a safe software-cursor fallback on other GPUs.
- Added process locking and old-instance cleanup before every launch.
- Added bilingual documentation, source, licensing and reproducible package
  audits.
