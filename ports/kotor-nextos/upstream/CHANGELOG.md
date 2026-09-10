# Changelog

## 1.1.7 — 2026-08-12

- Fixed the framework preflight abort on Knulli. The launcher now discovers
  Knulli's canonical `/userdata/system/.local/share/PortMaster` root and
  exports the exact sourced `controlfolder` as `NXCOMPAT_PORTMASTER_DIR`
  before launching the ARMHF runtime. This prevents `host.portmaster` from
  being reported missing after PortMaster itself had already initialized.
- Regenerated the visible launcher from canonical `nxbootstrap` 0.6.5. The
  game runtime remains byte-for-byte identical to v1.1.6, preserving the
  `sincos` softfp bridge fix, complete-period audio fill and all prior native
  lifecycle behavior.

## 1.1.6 — 2026-08-11

- Fixed the SIGSEGV when the Taris escape turret section loads (crash right
  after entering the Ebon Hawk, reported on RG34XX-SP/Knulli and reproduced
  on ArkOS). The guest's double-precision `sincos` import was the single
  libm entry point missing from the softfp bridge, so it resolved straight
  to the hard-float host libm and the out-pointers were read from the wrong
  registers, writing sine/cosine results through the raw bits of `x`. Added
  the `sf_sincos` AAPCS wrapper next to the existing `sincosf` one; audited
  every libm import of all seven guest modules against the bridge table and
  `sincos` was the only gap.
- Fixed PortMaster Auto Install: the ZIP now ships `kotor/port.json` with
  the complete attribute set (`runtime`, `reqs`, ...) that harbourmaster
  dereferences without defaults; the missing `runtime` key aborted the
  whole install on v1.1.5.
- Fixed choppy, delayed audio on CFWs whose ALSA negotiates a 1024-frame
  period (Powkiddy RGB30 on dArkOS): the mixer now fills the entire SDL
  audio period in 512-frame blocks instead of leaving half of every
  period silent.

## 1.1.5 — 2026-08-10

- Regenerated the self-contained launcher from canonical `nxbootstrap` 0.6.3
  commit `9d08f03`. Instance-lock validation now uses portable `ls -Lldn`
  metadata plus Bash `-ef` and no longer invokes the external `stat` command,
  which is absent on some muOS images.
- Preserved the approved ARMHF runtime, extraction recipe, native lifecycle,
  controller path and visible `GAME_LANGUAGE` selector byte for byte.

## 1.1.4 — 2026-08-10

- Consolidated the public launcher on the canonical `nxbootstrap` 0.6.3
  source, including the BusyBox-compatible instance lock proven by Swordigo
  on AmberELEC. The launcher no longer depends on GNU `stat -c`.
- Moved the visible game selector to `GAME_LANGUAGE="en"`; the framework
  validates it and exports `NXPORT_LANGUAGE` only to KOTOR's existing native
  language adapter. Linux locale and NXExtract remain unchanged.
- Synchronized the vendored release tools byte-for-byte with the canonical
  framework and kept the approved single ARMHF low-glibc runtime.

## 1.1.2 — 2026-08-10

- Updated the self-contained launcher to `nxbootstrap` 0.6.2. NXExtract now
  receives the physical game directory and the regular in-port
  `kotor/extractor.json` explicitly, preventing a stale or external inherited
  recipe path from breaking installation on ROCKNIX-style layouts.
- Added the standard visible `NXPORT_LANGUAGE` selector. The adapter maps
  `en`, `fr`, `it`, `de`, `es` and `pl` to the exact language IDs requested by
  Aspyr's original `KOTOR.getCurrentLanguage()` JNI flow; invalid values fall
  back to English.
- Consolidated every firmware on one low-glibc `kotor-nextos` runtime and
  linked controller normalization into it. The old host-glibc binary branch
  and optional input preload can no longer select different behavior between
  devices.
- Synchronized the approved framework sources, including capability-driven
  EGL-config evidence and `nxgl` 0.2.1 provider preservation. The game's
  constructors, `JNI_OnLoad`, Android lifecycle, OBB mount and `SDL_main`
  sequence remain unchanged.
- Added release gates that regenerate and compare the launcher/manifest,
  exercise the language map and audit every packaged Linux ELF against the
  public `GLIBC_2.30` ceiling.

## 1.1.1 — 2026-08-09 (branch claude)

- Replaced the generated giant launcher + nxbootstrap bash library with the
  single small PortMaster-canonical launcher produced by nxbootstrap 0.6.0
  (Limbo shape, 88 lines): official controlfolder chain plus the
  EmuELEC/NextOS path, control.txt + mod_CFW + get_controls, GAMEDIR from
  $directory with script-relative fallback, plain log.txt with no
  FAT-hostile gates, ESUDO exec-bit restore, NXExtract phase (both fleet
  layouts), armhf library and 32-bit audio module blocks, pad mapping
  export, pm_platform_helper, foreground game, pm_finish.
- run.sh and the bash runtime library are forbidden and absent; robustness
  (instance lock, payload gate, capability preflight) lives in the loader
  ELF where it already ran.
- KOTOR-specific pieces move to kotor/port-env.sh, sourced by the launcher:
  glibc-based runtime selection, the pad-normalizer BIN_PRELOAD, the OpenSL
  and high-res defaults and the extra external pad mappings.
- Motivation: the muOS RG40XX-H field report on 1.1.0 failed with no log at
  all; the library's FAT-hostile logging gates can die mute before any file
  exists. The new launcher writes log.txt unconditionally and every failure
  path speaks.

## 1.1.0

- Integrated the complete NextOS runtime framework while preserving KOTOR's
  native Android order: constructors, `JNI_OnLoad`, activity/resume, OBB mount
  and `SDL_main` remain in the proven sequence.
- Added capability-based `nxbootstrap` launch and NXExtract 1.2.6. The public
  ZIP now has the standard PortMaster launcher plus `kotor/`; the legacy
  `run.sh` layer is gone.
- Consolidated the loader, controller normalizer and framework into one public
  ARMHF executable named `kotor-nextos` (maximum requirement `GLIBC_2.28`).
- Added runtime evidence for the real engine-owned graphics context, opened
  audio device and active controller mapping. No second GL context, audio
  device or controller event loop is created by the framework.
- Kept the dynamic PortMaster/SDL mapping, unbound-button filtering,
  guide-button refusal, trigger handling, evdev fallback and Select+Start
  save-safe exit from 1.0.4.
- Moved privileged KMSDRM console restoration into `kotor-nextos` itself. On
  non-root CFWs the process runs its immutable `/proc` executable descriptor
  through passwordless `sudo -n`, so removing `run.sh` cannot leave the
  keyboard, controls or power button in SDL's `K_OFF` mode.
- Replaced the package recipe with a deterministic, BYO-data-only release gate
  that audits every ELF for `GLIBC <= 2.30`, rejects private data and proves
  that `run.sh` and the old preload library cannot re-enter the ZIP.
- Fixed `nxbootstrap` descriptor probes so checking executable/lock files never
  leaves the launcher's standard error redirected to `/dev/null`; runtime and
  framework diagnostics now remain in `debug.log` through the full handoff.
- Accepted the same release binary on real fbdev/Mali-450 and
  KMSDRM/Mali-G31 systems, including graphics, audio, controller discovery,
  extraction/adoption and clean console restoration.

## 1.0.4

Field report from an RG34XX-SP / Knulli tester: the hotkey button acted as
START (pausing and unpausing in the same press), the volume buttons did
nothing, and after quitting the game **no button on the device worked at all,
including power** — the handheld needed a hard reset.

- **A physical button with no binding no longer reaches the game.** The
  normalizer used to forward such a button with index `0xFF`, a garbage index
  into the engine's own tables. Pads whose hotkey emits two codes (e.g.
  `BTN_TL2` plus `KEY_GOTO`, both enumerated by SDL because `KEY_GOTO` falls
  inside `[BTN_JOYSTICK, KEY_MAX)`) produced two of those per press, which is
  what read as "pause, then unpause". Verified on the R36S: the mapping's
  unbound `b16` now logs `has no binding; event dropped` instead.
- **`guide:` bindings are refused**, in the launcher and in the normalizer.
  Mapping generators translate an `es_input.cfg` whose `hotkeyenable` shares a
  button with another function into `guide:bN` plus that other binding on the
  same `bN`; as mappings are written alphabetically, the later key silently
  won. A physical button already claimed by one key is no longer re-claimed by
  a later one either.
- **SELECT+START is also read straight from evdev**, covering
  `BTN_TRIGGER_HAPPY1..4`, for pads whose mapping declares no back/start at
  all (SDL computes a GUID with a CRC field that often fails to match the
  `gamecontrollerdb` line). This path stays off while the mapping is complete:
  some drivers report the physical LT/RT as `BTN_SELECT`/`BTN_START`, where an
  unconditional read would turn "both triggers" into "quit the game".
- **The console is always handed back.** On KMSDRM, SDL leaves the console
  keyboard in `K_OFF` and only restores it via `atexit` and its own fatal
  signal handlers — a list that excludes SIGTERM. A process killed, hung in
  teardown, or leaving through `_exit()` therefore left the whole device
  unresponsive, power button included. The loader now restores `KDSKBMODE` /
  `KDSETMODE` before every exit path. Because that ioctl needs privilege — a
  port running as an ordinary user gets `EPERM` — the binary also answers
  `--restore-console`, which the launcher calls through the CFW's sudo once the
  game is gone. Verified from a console forced into `K_OFF`/`KD_GRAPHICS` on
  both a root CFW and an unprivileged one.
- **Shutdown has a deadline.** SIGTERM is handled in the same path as
  SELECT+START (the game is asked to quit through SDL_QUIT), a failure while
  tearing the engine down becomes a clean exit instead of a crash status, and
  a watchdog guarantees the process leaves within
  `KOTOR_SHUTDOWN_DEADLINE` (8s default) rather than sitting on the display.
- **Volume follows the system volume where the CFW has no softvol**
  (batocera/Knulli remove `.asoundrc` in auto mode, so the raw card ignores the
  volume buttons). Opt-in via `KOTOR_SYSVOL=1`, enabled automatically only when
  the launcher detects that platform; `KOTOR_VOLUME_FILE`/`KOTOR_VOLUME_KEY`
  override the source.
- `run.sh` no longer discards a `SDL_GAMECONTROLLERCONFIG` exported by the
  user, and reads an optional `gamecontrollerdb.txt` placed next to the port —
  a per-pad fix no longer needs a rebuild. It also sets
  `SDL_GAMECONTROLLERCONFIG_FILE` from the firmware database and
  `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`, and supervises the game as a child
  so the frontend's SIGTERM reaches it.

## 1.0.3

Controller rework after the 1.0.2 field reports (muOS RG40XX-H: face and
shoulder buttons still dead; ROCKNIX RG DS: buttons fine but d-pad dead;
neither device could exit the game):

- The pad normalizer no longer guesses the physical layout from VID/PID
  profiles. `retrogame_joypad` is configured per firmware, so the same
  USB id ships different button orders (and a HAT d-pad) on different
  CFWs — the 1.0.2 assumption was wrong. The normalizer now builds its
  translation table at runtime from the controller mapping the firmware
  itself provides (PortMaster `get_controls` →
  `SDL_GAMECONTROLLERCONFIG`): face buttons, shoulders, triggers
  (button- or axis-based), analog axes, HAT d-pad and the real
  SELECT/START pair for the save-safe exit combo. The proven Twin
  PS2/USB and GO-Super static profiles remain as fallback for pads
  without a mapping, and `KOTOR_INPUT_DYNAMIC=0` restores the previous
  behavior.
- `run.sh` and the normalizer now log the mapping in `debug.log`
  (`cfw pad mapping: ...` / `kotor_input: ...`), so tester logs show
  exactly how the pad was translated.
- Fixed the Quick Save freeze (hard-lock reported on R36S/DarkOsRE,
  reproduced and verified on R36S/ArkOS): the engine ends directory
  enumeration with the Win32 pair `FindNextFileA()==0` +
  `GetLastError()==ERROR_NO_MORE_FILES`, but the loader's
  FindFirstFileA/FindNextFileA implementations never set the Aspyr
  compat layer's last-error, so overwriting an existing quick save spun
  the main thread forever in the retry loop. The loader now reports
  ERROR_NO_MORE_FILES / ERROR_FILE_NOT_FOUND / SUCCESS through the
  layer's own SetLastError. (The first quick save always worked — the
  empty saves/ folder fails at FindFirstFileA — which is why the freeze
  only hit the second save onwards.)

## 1.0.2

Second round of tester fixes (muOS ran with clear audio/video on 1.0.1;
ROCKNIX regressed to a permission error):

- `run.sh` now restores the executable bits of every shipped binary and
  script before launching (`chmod +x`): ZIPs extracted on Windows or FAT
  media lose the mode bits and the ext4 ROM partition then refuses to run
  the loader ("Permission denied", RG DS/ROCKNIX). Same lesson as
  Stardew v1.1.5. Also unlocks `$CUR_TTY`/`/dev/uinput` like our other
  published ports.
- The pad normalizer now recognises the `retrogame_joypad` controller of
  muOS H700 devices (RG40XX/RG35XX family) and ROCKNIX RG DS: same
  kernel driver and button order as the R36S GO-Super pad, only the USB
  product id differs (0x1101 vs 0x1100). This maps the face buttons,
  shoulders, triggers and SELECT/START (TRIGGER_HAPPY) to the Android
  layout the Aspyr engine expects — fixing "only d-pad and sticks work"
  and restoring the SELECT+START save-safe exit on those devices.

## 1.0.1

Launcher fixes for AArch64 firmwares (muOS RG40XX-H and ROCKNIX RG DS
tester reports — the game itself never got to run on either):

- `PORT_32BIT="Y"` exported by the visible launcher and by `run.sh` before
  the PortMaster hooks load: muOS inspects this literal marker to expose its
  32-bit GL/PipeWire runtime to the ARMHF process (TASM2 pattern). Without
  it the loader only saw the 64-bit `libGLESv2.so.2` (`wrong ELF class`).
- Library path now probes `/usr/lib32`, `/usr/local/lib32` and the
  PortMaster `libs`/`libs.armhf` folders ahead of `/usr/lib`.
- 32-bit ALSA/PipeWire/SPA module directories exported when present, and the
  PulseAudio socket probe also checks `$XDG_RUNTIME_DIR/pulse/native`
  (pipewire-pulse): fixes `SDL_Init Error: Could not connect to PulseAudio`
  aborting the game on ROCKNIX.
- The input-normalizer `LD_PRELOAD` is scoped to the game process only,
  ending the `wrong ELF class: ELFCLASS32` spam from 64-bit helper
  processes in the wrapper.

## 1.0.0 (draft — not yet released)

First universal release.

- ARMv7 so-loader for the Aspyr Android build 53 (1.0.10), running the
  original Odyssey/SDL2/GLES2 flow on the device's native GL stack.
- Public runtime built against Debian Buster: `GLIBC_2.27` ceiling, non-PIE,
  resolves against the firmware's 32-bit SDL2/EGL/GLESv2.
- Real-display resolution probe (DRM connector → fbdev → fallback), no
  hard-coded geometry.
- Per-pad input normalization profiles: Twin PS2/USB adapter (NextOS) and
  GO-Super Gamepad (R36S/R36T), including Select+Start save-safe exit.
- FMOD audio through the native OpenSL bridge over SDL.
- NXExtract 1.2.2 BYO-data installer: content-addressed recipe for APK +
  OBB cache, transactional install, resume, honest wrong-build diagnostics,
  fast marker on later launches.
- PortMaster-style launcher: thin visible wrapper, single-instance lock,
  `/proc`-proven cleanup, no frontend management, no forced SDL backends.

Validated physically on NextOS Elite (Mali-450/fbdev) and R36S/ArkOS
(Mali-G31/KMSDRM).
