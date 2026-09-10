# Changelog

## v1.2.2 — 2026-08-15

- Restored the published v1.2.0 control contract as the default: left-stick
  cursor, A touch DOWN/drag/UP, and right-stick plus D-pad board movement. The
  right-stick/R3 layout is now an explicit alternative only.
- Fixed the ArkOS/KMSDRM startup failure where SDL selected the correct video
  backend but its default versioned EGL provider could not create a GLES2
  window.
- The adapter now keeps the normal firmware/default attempt first and performs
  one clean SDL video reinitialization with the portable `libEGL.so` and
  `libGLESv2.so` provider names only after that attempt fails.
- Explicit `SDL_VIDEO_EGL_DRIVER` or `SDL_VIDEO_GL_DRIVER` settings remain
  authoritative and are never overwritten. Mali-450/fbdev keeps its existing
  raw-EGL path unchanged.
- Added host gates for normal selection, failed-window recovery, failed-init
  recovery, explicit-provider preservation, 1,000 repeated A drag cycles,
  focus/hotplug cancellation and cursor/swipe exclusion. The universal
  executable still requires at most `GLIBC_2.27`; owner data and the NXExtract
  recipe are unchanged.

## v1.2.1 — 2026-08-15

- Fixed touch dragging on strict Android/Unity input paths: joystick and touch
  no longer share one mutable `MotionEvent`, and `downTime` remains constant
  from DOWN through MOVE to UP/CANCEL.
- Added one touch owner for cursor, board swipe and UI shortcuts. Focus loss,
  minimize, controller hot-unplug and shutdown terminate an active gesture
  exactly once instead of leaving the next click stuck.
- Restored the framework control convention: right stick moves the polished
  cursor, R3 clicks/holds, left stick and D-pad move Agent 47. The v1.2.0
  A-click and swapped-stick layout remains opt-in.
- Migrated the public package to nxbootstrap 0.6.14, NXExtract 1.2.9,
  NXSplash 0.1.2, nxgenerator 0.2.8 and nxrelease 0.2.12.
- NXExtract now accepts differently signed/recompressed owner packages by
  package ID and exact critical payload identity, instead of one APK hash.
- NXExtract readiness evidence now stays in its private runtime directory,
  isolated from the game tree, while preserving the approved graphical UI.
- Public executable renamed to `hitmango-nextos`; removed RUNPATH and kept the
  maximum Linux ABI requirement at `GLIBC_2.27`.

## v1.2.0 — 2026-08-05

Control overhaul, validated in hands on an R36T-class device.

- **Rocks can be thrown again**: the tvOS/IL2CPP movement bridge turned out to
  be mutually exclusive with the game's touch input — with it selected, the
  board ignored node taps, so the rock aim opened but no target click ever
  landed. Board play now runs on the game's own touch manager and stays fully
  clickable; the tvOS bridge remains available via `HGO_NATIVE_CONTROLS=1`.
- **Board movement is a synthetic touch swipe** (the game's native swipe-to-move
  mechanic), steered by the D-pad or the right stick. `HGO_SWIPE_MOVE=0`
  disables it.
- **Cursor moved to the left stick**; movement lives on the right stick +
  D-pad. `HGO_SWAP_STICKS=0` restores the old sides.
- **A and R3 both click** at the cursor everywhere.
- No packaging, installer or compatibility changes: same NXExtract recipe,
  same raw-joystick/TRIGGER_HAPPY fallbacks, GLIBC ceiling unchanged (2.27).

## v1.1.1 — 2026-08-04

Field-report fixes (muOS/RG40XX-H and friends), same day as v1.1.0.

- **Pad outside SDL's built-in mapping database no longer loses ALL
  navigation** (muOS/RG40XX-H report: "the character won't move"): the loader
  now opens the pad as a raw joystick with positional button/axis order when
  no GameController mapping exists, and `run.sh` also feeds the CFW's
  `gamecontrollerdb.txt` to SDL when present.
- **NXExtract: a staged payload that fails whole-set validation is discarded**
  instead of being resumed and re-failing forever (local patch on top of
  1.2.1, recorded in `nxextract-version.txt`; candidate for upstream 1.2.2).
- **Recipe tolerances widened**: bigger XAPK/bundle member caps (3 GiB/6 GiB)
  and looser file-count floor so legitimate Play builds with slightly
  different packaging are not rejected as "different build".

## v1.1.0 — 2026-08-04

Compatibility review before the public release; parity with the fixes promoted
from the corrective releases of the other NextOS ports.

- **NXExtract 1.2.1 integrated (BYO-data installer)**: drop your legal Hitman
  GO 1.18.1 APK in `gamedata/` and the first launch validates, extracts and
  commits the data transactionally, with progress UI, resume and adoption of
  existing installs; your APK is never deleted. Fast marker validation on
  later launches (milliseconds, no SD rescan).
- **SELECT/START on pads without physical BTN_SELECT/BTN_START**
  (GO-Super/RK3326 family): the exit combo now also reads
  `BTN_TRIGGER_HAPPY1/2` via EV_KEY bitmap ordinals — additive probe, pads
  with real SELECT/START are untouched.
- **Unity RGBA8888 EGLConfig contract on Panfrost/Mesa** (RG-DS/ROCKNIX
  family): alpha 8 is requested first and the obtained config is logged once;
  Mesa returning RGBX8888 on the first match no longer risks a black screen
  (fix inherited from Horizon Chase v1.0.3).
- **SIGTERM/SIGINT converge on the SELECT+START shutdown**: pause, save and
  clean exit instead of a raw kill when the frontend or a supervisor sends
  TERM.
- Bilingual README with real photos, INSTALLATION.md, vendored-NXExtract
  version pins (`nxextract-version.txt`) and NXExtract license added.

## v1.0.0 — 2026-08-04

- Initial universal AArch64 release: Unity 2022.3.67f2 IL2CPP so-loader
  following the original Android lifecycle (constructors, `JNI_OnLoad`,
  `initJni`, surface, focus, resume, render, pause).
- All shipped ELFs require `GLIBC <= 2.30` (loader max: `GLIBC_2.27`);
  reproducible package build.
- Validated on NextOS Elite (Mali-450, fbdev/GLES2) and R36T-class
  ArkOS clone (Mali-G31, SDL/KMSDRM).
