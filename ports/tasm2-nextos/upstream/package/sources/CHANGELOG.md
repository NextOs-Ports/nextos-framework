# Changelog

## 1.1.9

- Ships a first-accept seed (`seed/first-accept/`): the two settings records
  the game itself writes after the first-run sequence, generated once on
  project-owned hardware. `run.sh` now plants them into the owner data
  directory when they are missing, so fresh and previously stuck installs
  skip the touch-only legal notice, update log and cloud notice entirely.
  Existing owner data is never overwritten, and the loader touch recovery
  remains as a fallback. This replaces per-panel screen-coordinate guessing,
  which failed on the 720×720 R36 Ultra even after 1.1.8's photographed
  position, because the game lays its dialogs out per panel aspect instead of
  scaling one canvas.
- Physical validation on NextOS Mali-450 at 1280×720: a clean install with
  only the seed planted boots with no first-run modals, reaches the title
  screen and starts the campaign from the beginning; a second boot recognizes
  the profile and leaves input untouched.
- Root-cause notes from the same session: the acceptance flag lives in
  `ud_OObjects.sav` (planting `ud_Control.sav` alone does not suppress the
  legal notice), and the post-acceptance cloud notice is confirmed to respond
  to the controller's logical A action.
- No loader changes: every bundled ELF is byte-identical to release 1.1.8.

## 1.1.8

- Fixes the first-run legal screen on near-square drawables such as 720×720 by
  targeting the photographed ACCEPT row instead of reusing the 4:3 vertical
  coordinate.
- Replaces the one-shot acceptance marker with a three-stage recovery state:
  legal touch, native-HID update-log confirmation and centered cloud-notice
  touch. Older markers migrate by drawable class, and incomplete attempts are
  retried until the game creates its own controls profile.
- Normalizes the update-log confirmation to the guest's logical A action, so
  Nintendo-style controller labels cannot turn the recovery press into B.
- Passed the complete clean-profile sequence on physical ArkOS at 640×480:
  legal, update log, cloud notice, controls and loading. A subsequent launch
  recognized the game-owned profile, left input unmodified and shut down
  cleanly.
- Rebuilds every bundled Linux ELF with a public ABI ceiling of GLIBC 2.30:
  ARMHF and Box32 require at most 2.28; the i386 loader, `sdl2-compat` and
  NXExtract UI require at most 2.17.

## 1.1.7

- Keeps SDL's inherited audio selection as the first choice on the external
  low-glibc ARMHF build. If automatic or inherited PulseAudio initialization
  fails inside that process, the OpenSL bridge retries once through the
  firmware's ALSA route. Arbitrary explicit diagnostic drivers remain
  untouched.
- Passed physical RG-DS validation on ROCKNIX with Panfrost, Wayland and Mesa
  26.1.2. The fallback opened the RK817 ALSA output and the owner reported
  clear audio; the recorded run completed 2,197 callbacks with one startup
  underrun (4,096 missing bytes), zero audio failures and clean shutdown.
- Records the ROCKNIX/Wayland first-install display limitation: NXExtract can
  complete its greater-than-1-GiB transaction while the screen remains black.
  Leave the device running and wait for the game; progress remains available
  in `debug.log`.
- Leaves the physically validated current NextOS ARMHF and X5M Box32
  executables byte-identical to release 1.1.6.

## 1.1.7-rc2

- Recognizes ROCKNIX's inherited `SDL_AUDIODRIVER=pulseaudio` selection as a
  server backend eligible for the existing ALSA retry when its connection
  fails inside the ARMHF process.
- Keeps arbitrary explicit diagnostic driver choices untouched and leaves the
  current NextOS ARMHF and X5M Box32 binaries unchanged.
- Replaces RC1, whose conservative explicit-driver guard correctly diagnosed
  PulseAudio but prevented the intended fallback from running.

## 1.1.7-rc1

- Keeps SDL's inherited audio selection as the first choice on the external
  ARMHF build. If that initialization fails and no explicit driver override
  exists, the OpenSL bridge retries once through the firmware's ALSA route.
- Targets the observed ROCKNIX ARM32 failure where PulseAudio could not be
  reached while gameplay and Wayland/Mesa video remained healthy.
- Leaves the current NextOS ARMHF and X5M Box32 binaries unchanged. This
  candidate is pending focused physical audio validation on ROCKNIX.

## 1.1.6

- Resolves ARMHF GLES imports on Mesa/ROCKNIX by looking up
  `glMapBufferOES` and `glUnmapBufferOES` lazily, with their GLES core names
  as compatible aliases. The existing i386/Box32 route remains unchanged.
- Marks the PortMaster entry point as a 32-bit port and selects the firmware's
  existing ARMHF ALSA, PipeWire and SPA module directories without bundling
  firmware libraries or forcing an SDL audio driver.
- A physical RG 40XX-H running muOS completed gameplay with clear ALSA audio
  and clean shutdown. The recorded run reached 6,222 audio callbacks with zero
  underruns, missing bytes or failures.
- Adds privacy-safe owner-source diagnostics: rejected direct APK candidates
  now report byte size, SHA-256 and the precise rejection reason without
  exposing filenames, source sites or local paths.

## 1.1.6-rc3

- Keeps the ROCKNIX GLES compatibility and muOS ARMHF audio environment fixes
  from the two previous release candidates.
- Logs every direct APK candidate's byte size, SHA-256 and precise rejection
  reason before the preparation hook exits.
- Logs accepted and supported profile identifiers without exposing the
  owner's filename, source site or local path.

## 1.1.6-rc2

- Keeps the ARMHF GLES OES/core compatibility fix from `1.1.6-rc1`.
- Marks the PortMaster entry point as a 32-bit port so muOS can prepare its
  ARMHF PipeWire and SPA runtime before launch.
- Selects existing ARMHF ALSA, PipeWire and SPA module directories when the
  firmware provides them. No firmware audio library is bundled and no SDL
  audio driver is forced.

## 1.1.6-rc1

- Resolves the ARMHF guest imports `glMapBufferOES` and
  `glUnmapBufferOES` lazily through the GLES stack selected by SDL.
- Tries the OES entry points first and then their GLES core aliases, covering
  Mesa/ROCKNIX stacks that do not export the OES names at link time.
- Keeps the existing i386/Box32 route unchanged. This candidate is intended
  for focused physical validation before a stable release.

## 1.1.5

- Accepts four audited owner-input profiles: two Android 1.2.7d APK layouts,
  one universal ARMv7+x86 1.2.8d APK and one self-contained ARM32-only 1.2.8d
  installer.
- Accepts the matching cache as loose OBB files, a companion ZIP or the
  validated self-contained installer. `main.12032` and `patch.12723` remain
  byte-exact requirements; `patch.12438` is now optional and is still fully
  validated whenever present.
- Replaces whole-container rejection at the preparation hook with explicit
  profile and native-library validation, fixing first-run failures that
  occurred after NXExtract had already staged valid data.
- Keeps the X5M route strict: ARM32-only input is rejected with a clear
  compatibility message, while all inputs carrying the exact x86 library
  remain eligible for Box32.
- Validates all four preparation profiles, both transactional input layouts,
  existing-install adoption, ARM32-only x86 rejection and real Mali-450
  gameplay without the optional older patch OBB. A physical Mali-450 also
  completed all four transactional ARMv7 extraction profiles, including a
  successful resume after interruption.

## 1.1.4

- Clarifies everywhere that the game is not APK-only: installation requires
  one exact Android 1.2.7d APK plus the complete three-file OBB cache.
- Adds a bilingual `INSTALLATION.md` inside the port and lists the exact
  `main.12032`, `patch.12438` and `patch.12723` filenames.
- Tells users to leave the OBB files intact in `gamedata`; no runtime binary,
  extractor recipe or generated-data contract changed from 1.1.3.

## 1.1.3

- Keeps the packaged X5M `sdl2-compat` private to the Box32 game process.
  NXExtract now starts with the X5M firmware SDL2/KMSDRM stack, matching the
  proven Horizon Chase first-run handoff and preventing a black extractor UI.
- Adds an extractor-only physical validation gate for maintainers; it can
  complete and publish a clean first-run transaction without starting
  gameplay, and is never enabled in the public launcher.
- Extends the deterministic package contract and provenance checks so a future
  build cannot leak the game SDL into the extractor environment.

## 1.1.2

- Aligns the X5M launch lifecycle with the 100%-working Horizon Chase port:
  EmulationStation/PortMaster owns DRM handoff, while the complete game
  launcher remains in the foreground.
- Stops handing the raw Box32 host to a platform helper. That helper cannot
  preserve the required i386 guest arguments and scoped runtime environment.
- Closes the universal release gate after deterministic package/source builds,
  reinstall/update preservation, full extractor validation on all three
  physical targets and exact-package ARM smoke tests.

## 1.1.1

- Makes the normalized runtime APK byte-identical across firmware zlib
  versions by storing all 622 already-recovered members without compression.
  Full per-member CRC/SHA-256 verification and the final archive hash remain
  mandatory before NXExtract can publish the transaction.
- Bumps the extraction recipe so an older compressed runtime APK cannot satisfy
  the new portable marker.

## 1.1.0

- Adds the validated AArch64 NextOS X5M route: the owner's original Android
  x86 library runs through a narrowly scoped Box32 host and `sdl2-compat` on
  the `amlogic,s7d` / Mali-G310 platform. The required scoped profile is
  `DYNAREC=1`, `BIGBLOCK=0`, `SAFEFLAGS=2`; long gameplay,
  save/create/update/reload and reopen completed with RC0.
- Extends NXExtract's single transaction to recover and validate both ARMv7
  and x86 native game libraries. An older ARM-only marker cannot skip the new
  output.
- Keeps the two proven ARMHF routes: a current-NextOS build and a low-glibc
  PortMaster build selected at runtime.
- Preserves the native first-run order, including the one-shot legal touch,
  cloud-data notice, control presentation, progressive loading and gameplay.
- Adds exact runtime hash/dependency preflight, corresponding source and
  license material for Box64 and `sdl2-compat`.
- Replaces catalogue artwork with a real 640×480 R36S gameplay capture and
  records its provenance.

## 1.0.1

- Preserves the original Android exit/restart lifecycle: `sExitGame()` invokes
  the game's `GameOptions.onExit()` callback once, while `RestartGame()` exits
  with status 75 and the launcher performs at most one foreground restart.
- Fixes a version-locked first-profile teardown crash seen after a genuinely
  empty installation, without skipping any native startup or loading stage.
- Adds an atomic launcher lock and verifies that old game processes are gone
  before starting, including executables replaced while still running.
- Validates the selected loader as ARMHF, verifies its declared interpreter
  and resolves all runtime libraries before extraction or game startup.
- Adds ARMHF library paths used by minimal PortMaster firmwares and accepts
  Wayland only when the corresponding socket actually exists.
- Refreshes both loader builds and records their exact hashes in
  `BUILD-PROVENANCE.json`.

## 1.0.0

- Public BYO-data ARMHF package for Android 1.2.7d.
- Separate current-NextOS and low-glibc PortMaster loaders, selected at run
  time without forcing an SDL video or audio backend.
- Transactional first-run preparation through NXExtract 1.1.2 with a
  downstream fix for primary loose files whose extension was removed.
- Exact validation of the source APK, three OBBs, recovered ARMv7 library,
  rebuilt runtime APK and generated offline shop catalogs.
- Updates commit only the seven generated proprietary outputs; saves,
  preferences, cache and the owner's source packages stay outside the
  transaction and are preserved.
- First physical controller action can accept the original first-run legal
  disclaimer through the loader's one-shot touch bridge.
