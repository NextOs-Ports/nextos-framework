# Changelog

## 2.0.2

- Recorded the second game build verified on hardware: Terraria Android
  1.4.5.8.5 (versionCode 301709). The 2.0.1 package accepts and runs it with
  no code change: NXExtract classifies it as an unknown compatible build,
  fixed-offset hooks stay disabled, and every menu hook resolves by IL2CPP
  name at the new addresses.
- Verified on two devices on 2026-09-08: NextOS Retro Elite (Amlogic,
  Mali-450, AArch64) and R36S/dArkOS. Both extract the `.apkm` cleanly
  (`NXE0000`) and reach the menus; on the Mali-450 box the run also creates a
  character and generates and saves a Large world.
- Documentation only: the loader bytes are unchanged from 2.0.1, whose ZIP
  rebuilds byte-for-byte identical
  (`5682053bc0614283daed3ff10f13d9b7327392c49d3b22a95fd0548e7a4c1576`).

## 2.0.1

- Removed the exact dotted game-version token as an APK acceptance gate. The
  2.0.0 hook incorrectly rejected the documented reference payload because its
  internal asset records `1.4.5.6.49`, not a standalone `1.4.5.6.4` token.
- Compatible owner data is now accepted by package family, AArch64 ABI,
  required Unity/IL2CPP structure, file signatures, engine compatibility and
  payload coherence. Container SHA-256, size, versionCode and exact version
  text remain provenance only and do not reject a compatible build.
- Known internal-library hashes only identify the reference build and enable
  its guarded fixed-offset fallbacks. Unknown compatible builds keep the
  generic symbol/signature-resolved runtime path.
- Added regression fixtures for the real `.49` internal suffix, a compatible
  payload with no dotted game-version token, and rejection of an incompatible
  Unity engine family.

## 2.0.0

- Migrated the public package by opt-in to the immutable
  `framework-v2-onda1` release: generated nxbootstrap 0.6.30 launcher,
  NXExtract 1.2.18, mandatory five-second NXSplash 0.1.2 and NXRelease 0.2.30
  ZIP gates. The port pins commit and component tree hashes in
  `FRAMEWORK-PIN.json`.
- Fixed Unity's Android orientation contract. The JNI layer now returns the
  real `ActivityInfo.SCREEN_ORIENTATION_*` values and retains coherent
  `getRequestedOrientation`/`setRequestedOrientation` state instead of
  aliasing every state to landscape (`0`).
- Added EGL-provider ownership detection. A PowerVR/Imagination provider
  exposed through a historically named `mali` SDL backend now uses an
  SDL-owned native window; proven ARM/Mali fbdev and normal KMSDRM/Wayland/X11
  paths keep their existing behavior.
- Added one measured SDL provider fallback after a real window/context failure.
  It reinitializes video once with portable EGL/GLES sonames only when the
  firmware/user supplied no provider override; explicit settings remain
  authoritative.
- Bounded exact repeats of `EGL_BAD_NATIVE_WINDOW` and long GL-extension dumps
  after their initial diagnostic copies, and stopped logging the per-frame
  Choreographer transport callback. A failed graphics retry or normal long
  session can no longer grow `log.txt` without limit.
- Accepts compatible single APK, split APK set, APKM, APKS and XAPK containers
  independently of filename, signature and outer SHA-256. Package family,
  AArch64 ABI, Unity/IL2CPP structure and the exact game-version anchor inside
  `data.unity3d` still fail closed.
- Added deterministic orientation/provider fixtures and fixed the low-glibc
  build recipe to reject incomplete stale sysroots before selecting headers.
- Renamed the public project executable to the canonical `terraria-nextos`.
  Owner data, players, worlds and user settings remain outside generated
  files and survive updates.

## 1.1.4

- Character creation no longer crashes on non-reference Play Store builds.
  The player-name hooks still called the reference build's fixed offset for
  `Main.get_PendingPlayer`; on any other build that jumps into the middle of
  an unrelated function (reproduced on device with Play build 301544:
  SIGSEGV the moment a new character is confirmed — the first thing a fresh
  install does after the menus, which is exactly where 1.1.3's validation
  stopped). The getter is now resolved by name through the IL2CPP metadata,
  keeping the fixed offset only as a reference-build fallback.
- Real `sincos`/`sincosf`. The generated import stubs returned without
  writing their output pointers, feeding stack garbage to every native
  rotation Unity computes from them (trail, particle and sprite
  transforms). Unity logged `IsNan NaN,NaN` and on some GPU stacks
  heavy-effect weapons corrupted the whole screen (reported with the Zenith
  on a Mali-G52 handheld). Both now route to the C library's real
  implementations; weapon swing and trail animation also compute correctly.
- The controller bridge no longer falls back to fixed offsets from an old
  1.4.4&#46;9 dump when name resolution transiently fails; it retries on the
  next frame instead of patching arbitrary engine code.
- The "unknown build" log line no longer spills raw manifest JSON after the
  build id.

## 1.1.3

- Boot every accepted Play Store build, not just the reference binary. The
  1.1.2 installer already accepted any structurally sound 1.4.5&#46;6.4 payload,
  but the loader still crashed at boot on builds whose libraries are laid
  out differently (verified on device with the Play build 301544, both as an
  `.apkm` and `.xapk` containers). Four fixes, all replacing
  reference-only assumptions:
  - shim `fstat` — newer-NDK builds import it directly and glibc only
    exports it from 2.33 on, so the unresolved slot jumped to garbage on
    the first file check of Unity's init;
  - resolve `fmodGetInfo` through the FMOD JNI registration by name (the
    fixed engine offset only exists in the reference build); audio still
    plays at the mixer's real rate on any build;
  - answer `getaddrinfo` with an honest failure (the old stub reported
    success without results, so builds that ship with Unity's performance
    reporter enabled dereferenced uninitialized memory during startup);
  - resolve the IL2CPP C API by symbol name in the controller bridge and
    the frame driver instead of fixed offsets.
- `.apkm` and `.xapk` containers verified end to end on device, alongside
  single APKs, `.apks` bundles and merged repacks.

## 1.1.2

- Fixed the installer rejecting every legitimate APK with "unsupported Unity
  engine 2018.3.0a1". `libunity.so` embeds historical version constants in
  its serialization compatibility tables besides the real engine version,
  and the placeholder can sit at a lower file offset; the validator took the
  first version-shaped string it found and refused the build. It now
  collects every version string and picks the real engine version, so any
  Unity 2021.3 build of Terraria 1.4.5&#46;6.4 installs again. No changes to the
  game loader itself.

## 1.1.1

- Gave the fullscreen map real controller support. The mobile build only ever
  zoomed the map by touch pinch (`GUIZoom.UpdatePinchZoom`) and its zoom
  buttons are touch UI that the game hides while a controller is active, so on
  a handheld the map opened but could not be zoomed. The loader now drives
  `Main.mapFullscreenScale` and `Main.mapFullscreenPos` directly: `LT`/`RT`
  zoom out/in and the right stick pans. The fields are resolved by name
  through the IL2CPP API, so this works on any accepted build. Disable with
  `TER_NOMAPCTL=1`.

## 1.1.0

- Accept every structurally sound Android build of Terraria 1.4.5&#46;6.4, not
  just the single reference build. Google Play ships more than one build of
  the same visible version (and tools like AntiSplit-M repack them), so the
  recipe now validates structure — package layout, ELF/ABI, UnityFS and
  IL2CPP metadata signatures, Unity 2021.3 engine — instead of one exact
  SHA-256 per file. The reference build is still recognized by hash and gets
  the full patch set; unknown builds are labelled in `.terraria-data.json`.
- On unknown builds the loader only applies patches whose target bytes it can
  verify first. The IL2CPP C API is now resolved by exported symbol name
  (build-independent); the storage-space bypass verifies it is really looking
  at the expected `tbz` before NOPing it; the Quit-Game bridge degrades to a
  log line instead of refusing to start (`SELECT+START` always exits);
  RVA-only extras (auto-name enter hook, audio stream fallback) switch off
  with a clear message instead of patching blind.
- Protect saves against truncation: writable files are fsynced on close, every
  abnormal exit path (`crash`, watchdog timeout) now calls `sync()` before
  leaving, and the final quit watchdog allows 15 s (was 5 s) so a slow SD card
  can finish writing the world before teardown. A truncated `.map`/`.wld` is
  the reported "world only loads with the minimap disabled" failure.
- Always report silent depth/stencil downgrades and incomplete framebuffers
  in `run.log` (previously diagnostic-only), to pin down the "minimap
  sometimes does not open" report on GLES2 drivers.
- NXExtract 1.2.2: a failed extraction can no longer leave the fullscreen
  setup UI holding the display, input and virtual terminal — the reported
  "R36S cannot shut down safely after a failed setup". The launcher also
  terminates any stray setup UI (matched by its working directory) and resets
  the tty on the error path.

## 1.0.3

- A fully installed game is no longer reported as a failed data setup.
  NXExtract 1.2.1 drops its scratch source cache only after every source
  archive is closed, and a removal the filesystem still refuses is logged
  instead of aborting the run. On FUSE-backed shares -- exFAT as Knulli and
  Batocera use it for `/userdata`, plus NFS and SMB -- a file unlinked while
  still open leaves a hidden placeholder behind, so `source-cache/bundle-*`
  answered `[Errno 39] Directory not empty` seconds after the payload had been
  committed and validated, and the launcher stopped with a data-setup error
  even though the extraction had finished correctly.
- The loader binary is unchanged from 1.0.2. This release touches the data
  extractor only.

## 1.0.2

- Connected Terraria's four-byte no-op `Game.Exit` method to the loader's
  guarded teardown, after the game's original settings-save and social
  shutdown steps have run.
- Honored the engine-owned `false` return from `nativeRender` as a second
  native Unity exit path instead of calling one more frame.
- Kept the already-validated immediate `SELECT+START` exit path unchanged.
- Added self-contained bilingual package documentation that does not depend on
  repository-only images or a release ZIP's own hash.

## 1.0.1

- Made `SELECT+START` exit immediately instead of requiring a 750 ms hold.
- Accepted firmware mappings that expose Select as either Back or Guide, with
  a device-scoped raw fallback for the GO-Super controller.
- Added a three-second process watchdog while preserving Terraria's native
  focus-loss and pause callbacks on the normal exit path.

## 1.0.0

- Rebuilt the Android Unity/IL2CPP loader as one public AArch64 binary requiring
  at most GLIBC_2.27.
- Removed firmware-specific absolute game paths and `/dev/dri` device guesses.
- Added capability-based SDL/raw-EGL ownership and real drawable sizing.
- Unified controller and Terraria frame hooks across both presentation paths.
- Added a Terraria-themed QWERTY controller keyboard with row-aware horizontal
  navigation and reliable `DONE` handling on the managed game thread.
- Preserved Terraria's original `EnterName` -> `CloseNameEdit` -> Create flow
  for player and world naming instead of invoking creation out of order.
- Added lifecycle-aware hotkey, SDL quit and signal shutdown.
- Added deterministic BYO-data installation for Android 1.4.5&#46;6.4 with
  NXExtract 1.2.0.
- Added complete source, license notices, deterministic packaging and release
  gates for all included ELFs.
