# The Amazing Spider-Man 2 — Android 1.2.7d runtime, 1.2.7d/1.2.8d owner inputs

**Language / Idioma:** [English](#english) · [Português](#português)

This directory targets the exact Android **1.2.7d native runtime** (`versionCode
12723`, package `com.gameloft.android.ANMP.GloftASHM`). Release 1.1.8 accepts
four audited Android 1.2.7d/1.2.8d owner containers that carry that same exact
runtime. Other APK, native-library and expansion variants are not
interchangeable.

It supports the original ARMv7 library natively and the original Android x86
library through the physically validated AArch64 X5M Box32 route. The public
package is bring-your-own-data: it contains no APK, OBB or game library. One
supported self-contained 1.2.8d input has ARMv7 data only and is therefore
limited to ARM32/multilib systems.

Este diretório usa o **runtime nativo Android 1.2.7d** exato (`versionCode
12723`, pacote `com.gameloft.android.ANMP.GloftASHM`). A release 1.1.8 aceita
quatro contêineres Android 1.2.7d/1.2.8d auditados que carregam exatamente esse
mesmo runtime. Outras variantes de APK, biblioteca nativa e expansões não podem
ser misturadas.

Ele suporta a biblioteca ARMv7 original de forma nativa e a biblioteca Android
x86 original pela rota Box32 validada fisicamente no X5M AArch64. O pacote
público exige os dados do próprio usuário e não inclui APK, OBB nem biblioteca
do jogo. Um dos insumos autocontidos 1.2.8d possui somente dados ARMv7 e, por
isso, funciona apenas em sistemas ARM32/multilib.

---

## English

This is an independent compatibility port of The Amazing Spider-Man 2. A
custom ELF loader runs the original `libtasm2.so` directly on ARMHF without an
Android userspace or Java VM. On the validated AArch64 X5M route, a separate
i386 loader runs the original `libtasm2-x86.so` through Box32 and
`sdl2-compat`. Both routes supply the Bionic, JNI, lifecycle, storage, audio,
video and input surfaces required by this exact game binary.

Status: **PLAYABLE on every physically validated route.** Gameplay, controls,
audio and clean shutdown were verified on NextOS/Mali-450, ArkOS/R36S,
muOS/RG 40XX-H, ROCKNIX/RG-DS and NextOS/X5M; the original target matrix also
passed checkpoint creation and save reload. A fresh data tree completed the
original legal/terms, update-notice, native cloud-data notice and
controller-title flow before entering the first level. The notice does not
request a mandatory download and disappears after the profile completes the
first-run sequence.

Release 1.1.8 replaces the old one-shot legal conversion with a persisted
three-stage recovery sequence: drawable-aware legal touch, native HID for the
update log and a centered touch for either cloud notice variant. The 720×720
layout uses the ACCEPT row measured from the reporter's screenshot. Legacy
markers are migrated by drawable class, and only the game-owned
`ud_Control.sav` makes completion permanent. The complete sequence and the
next-launch no-interception path passed on physical ArkOS at 640×480. The
update-log stage normalizes every face-button trigger to the guest's logical A,
covering Nintendo-style controllers whose printed A/B labels differ from SDL.

The first validation session created a checkpoint and exited cleanly after
48,400 frames. Restarting with the same profile skipped terms, update and cloud
notices, returned to the menu, reloaded the mission and ran another 9,777
frames. The stripped baseline binary then passed launcher-driven tests with
both a fresh profile and an existing profile, covering title, menu, mission,
tutorial and clean exit back through the launcher.

Release **R2** retains those protections and is hardware-validated through
mission 2. Audio remained stable across the mission transition and extended
gameplay, the offline item-and-suit shop opened and remained navigable, and a
standard Xbox-layout controller operated gameplay, camera and the title,
options, pause and shop menus.

### Clean-room boundary

The implementation was produced only from:

- the audited user-owned 1.2.7d/1.2.8d Android containers and matching
  expansion files;
- generic, game-independent loader components already present in this
  repository;
- public platform/API documentation; and
- conceptual lessons from completed, working ports.

No previous implementation of this game port is a source for this directory.
Game-specific offsets and behavior are checked against the exact 1.2.7d native
library shared by those audited containers and fail closed when its layout
does not match. See [`CLEANROOM.md`](CLEANROOM.md).

### Architecture

1. `so_util.c` maps the ARM32 ELF while `so_util_x86.c` independently maps the
   i386 ELF. Both apply Android `REL` relocations, resolve imports and execute
   the guest constructors.
2. `imports.c` routes ABI-sensitive symbols through explicit bridges. Plain
   pointer/integer libc and GLES2 functions can fall back to the host.
3. `bionic_compat.c`, `pthread_bridge.c`, `softfp_bridge.c` and
   `setjmp_bridge.S` translate old ARM32 Bionic layouts and calling conventions
   to the NextOS glibc/hard-float environment.
4. `jni_bridge.c` provides a self-contained JNI 1.6 VM/environment. The Android
   callbacks reproduce only the Activity, device, path, preference and
   `DataSharing` behavior needed by version 1.2.7d.
5. `main.c` drives the original exported JNI entry points in Android lifecycle
   order, then owns the SDL event/render loop.
6. `video.c` creates a fullscreen SDL/EGL/GLES context; `input.c` forwards
   native controller HID events, Android key codes and touch events. The X5M
   route resolves guest GL calls lazily through SDL after its real context
   exists.
7. `opensl_bridge.c` implements the OpenSL ES engine/output-mix/player/buffer-
   queue surface over SDL2 audio. The game's requested S16LE PCM is queued
   without entering guest callbacks while an audio mutex is held.
8. Android external-storage paths are virtualized below `gamefiles/`, keeping
   all original data, saves and cache beside the port.

The release deterministically uses the game's own native offline startup path.
The 2014 Gameloft EVE profile transaction no longer completes: with a live
network link the guest retries at 45% even when the service root is reachable.
The platform callback therefore reports no connection, letting the original
guest state machine take its complete built-in offline branch.

Advertising, browser, notification and social integrations are not recreated.
Their Android callbacks return safe offline values or no-op where appropriate.

### Compatibility walls solved

| Wall | Cause | Solution |
|---|---|---|
| APK rejected by normal ZIP readers | shifted central-directory offsets and one missing raw-DEFLATE byte in the known 1.2.7d input | recover from physical local headers, validate size/CRC, then build a separate standards-compliant runtime APK |
| intermittent ZIP crash and progressively slower file access | the old guest libzip reads fields from an 84-byte Bionic `FILE`, but glibc exposes a different opaque object; a historical linear registry made every lookup slower | synchronized guest wrappers in a hashed active registry, with stale-handle tombstones kept outside the hot lookup path |
| mutex/condition/semaphore corruption, stuck timeouts and growing lookup cost | old Bionic synchronization objects are smaller than glibc objects, the game builds absolute deadlines from `gettimeofday`, and retired bridge objects accumulated in active buckets | real host pthread/sem objects, Bionic-compatible realtime condition waits, hashed active-only buckets and separate retired tombstones |
| bad float arguments/results | the guest uses base AAPCS while the NextOS toolchain is ARM hard-float | explicit softfp-to-hardfp math and GLES bridges |
| missing Java/Activity bootstrap | the game expects JNI 1.6, installer state and a precise native-init sequence | fake VM/environment, registered-native dispatch, version-locked installer postcondition and ordered lifecycle driver |
| crash when a controller connects during startup | the controller listener reaches engine objects created by the first GL frame | render one warm frame before opening the SDL controller |
| audio can remain silent after a transition | the original two-buffer stream can reach an empty absorbing state when a completion callback misses its refill window | timed one-at-a-time refill recovery, reusable PCM buffers and high-priority best-effort callback worker |
| audio teardown can deadlock or outlive its guest context | the 1.2.7d destructor holds the mixer mutex across `Destroy`, while the completion callback needs that same mutex | version-checked mutex handoff, synchronous external join and worker-owned cleanup only for self-destruction |
| ROCKNIX ARMHF gameplay has no audio | its 32-bit SDL inherits PulseAudio, but that server is unavailable to the ARMHF process while the firmware's RK817 ALSA route remains usable | on the external low-glibc build only, preserve the inherited choice first and retry ALSA once after automatic or inherited PulseAudio initialization fails |
| Android paths absent | the engine uses `/sdcard`, app-private data, cache, APK and OBB paths | map Android storage prefixes into the local `gamefiles/` tree |
| first-run settings lost after exit | Android `DataSharing` normally persists through Java | mutex-serialized bounded binary store with CRC, atomic temporary-file/rename commit, identical-value suppression and rollback on write failure |
| first-run dialogs cannot both close | the offline data notice and `UI_FIRST_CHECK` use the same `ConfirmBoxSYS` in one frame, so the later callback replaces the visible dialog's callback | defer only the second builder, then restore the original version-checked guest instruction and let the game create it on the next frame |
| square-panel legal ACCEPT does not react and later cloud notices can also trap input | the old conversion reused the 4:3 Y coordinate on 720×720 and permanently marked the attempt before the game proved that setup had completed | use layout-aware legal/cloud coordinates, native HID for the update-log step, migrate legacy markers and treat the game-owned controls profile as the only permanent completion proof |
| live network stalls at 45% | the legacy EVE profile transaction does not complete even though its root endpoint is reachable | report no connection deterministically so the guest takes its complete native offline branch |
| unsafe guest GL teardown after an active session | this engine walks stale process-lifetime objects in `GL2JNILib_destroy` | execute pause/options/input shutdown and let normal process exit reclaim guest-global objects |
| extension entry points missing as link-time exports | Mali userspace exposes some GLES extensions only through the runtime resolver | extension lookup and GLES2-compatible platform shims |
| i386 GL calls intermittently crash under the default dynarec | the first lazy GL bridge entered C with a misaligned i386 stack, and the X5M driver path also needs stricter dynarec block/flags handling | preserve the SysV i386 16-byte call-site ABI and scope `DYNAREC=1`, `BIGBLOCK=0`, `SAFEFLAGS=2` to the exact tested X5M route |
| AArch64 X5M has no ARMHF userspace | the device cannot execute the native ARMv7 loader or game library | run the APK's genuine Android x86 library through a source-built Box32 host and AArch64 `sdl2-compat`, while keeping the Android lifecycle unchanged |
| X5M first-run extractor renders black | the native AArch64 UI inherited the game-only `sdl2-compat`, which cannot acquire the pre-Box32 EmulationStation KMS handoff | use the firmware SDL2/KMSDRM stack for NXExtract and scope packaged `sdl2-compat` only to the Box32 game invocation; the public-package contract test rejects either path leaking into the other |

### Audio

The Android library imports OpenSL ES 1.0-style object, engine, play and buffer-
queue interfaces. The bridge copies enqueued PCM into reusable host-owned
buffers, SDL's audio callback drains them, and a separate worker invokes the
game's completion callback. A dry queue is polled conservatively and schedules
at most one recovery callback at a time, retrying every 50 ms until the guest
refills it. No allocation or guest call occurs in SDL's real-time callback.

The exact 1.2.7d audio destructor and completion callback use the same guest
mutex. Their instructions and callback prologue are checked before execution.
When teardown catches a callback in flight, the pthread bridge temporarily
hands that mutex to the callback, joins the worker, then reacquires it before
`Destroy` returns. This preserves the guest's original exclusion while avoiding
both a circular wait and a callback touching a freed context.

Five-second OpenSL state reports expose queue depth, dry/recovery callbacks,
underruns, missing bytes, stale enqueues and PCM peak. Fifteen-second runtime
reports expose process memory/swap, active-registry growth, JNI allocation and
graphics-fence fallbacks without scanning the process heap.

The launcher discovers an available PulseAudio socket and otherwise leaves SDL
to select the system audio backend and default output device. On the external
low-glibc ARMHF build only, the OpenSL bridge retries ALSA once if automatic or
inherited PulseAudio initialization fails. Arbitrary explicit diagnostic driver
choices remain untouched, and the current NextOS ARMHF and X5M binaries do not
enable this fallback.

### Controls

Standard Xbox-layout controls are hardware-validated. The game receives its
original Android controller events; no gameplay cursor or desktop key mapper is
required.

Controller events use the original `NativeBridgeHIDControllers` identifiers.
The official controller screen assigns the main gameplay actions below; menus
and scripted contexts can reuse the same buttons.

| Physical input | Guest input / official action |
|---|---|
| Left stick | movement, with a 12% dead zone |
| Right stick | camera, with a 12% dead zone |
| R2 | continuous right trigger; press to jump, hold to swing |
| L2 | continuous left trigger; special combo |
| X | attack |
| B | shoot web |
| Y | dodge |
| A | next dialogue |
| L1 / R1 | left/right shoulder; special action |
| D-pad | up, down, left and right HID buttons |
| Stick clicks | left/right stick button |
| Start / Select / Guide | Start, Select and Back; Start opens pause |
| Select + Start | cleanly exits the port |
| Touchscreen | up to ten native touch pointers |
| Left mouse button | touch down/drag/up, useful for touch-first menus |

Keyboard fallback:

| Keys | Guest input |
|---|---|
| Arrow keys | D-pad |
| `Z` / `X` / `A` / `S` | A / B / X / Y |
| `Q` / `W` | L1 / R1 |
| `1` / `2` | L2 / R2 |
| Left / right Shift | left/right stick click |
| Space / Tab / Home | Start / Select / Guide |
| Enter | Android Enter |
| Escape or Backspace | Android Back |

### Hardware release validation

Physical gameplay baselines cover:

- NextOS/Mali-450 using the ARMHF runtime source retained here;
- ArkOS/R36S using the exact public v1.1.8 low-glibc ARMHF loader;
- muOS/RG 40XX-H using the firmware's 32-bit ALSA, PipeWire and SPA modules;
- ROCKNIX/RG-DS using Panfrost, Wayland, Mesa 26.1.2 and the scoped ALSA
  fallback; and
- NextOS/X5M/Mali-G310 using the aligned i386 loader, the scoped Box32 dynarec
  profile and KMSDRM/GLES.

The final X5M validation completed 11,034 gameplay frames and a 6,213-frame
reopen, both with exit code zero. It created, updated and reloaded the save,
kept audio free of callback failures and used the physical controller. The ARM
targets likewise completed the native first-run sequence, progressive loading,
gameplay and save reload.

For v1.1.8, the X5M binaries were rebuilt twice byte-identically below the
GLIBC 2.30 ceiling. The unchanged Box32 host source/patch/safety profile starts
under AArch64 QEMU and the rebuilt i386 loader reaches its expected missing
owner-library boundary there; those exact rebuilt X5M bytes were not rerun on
the physical board.

On ROCKNIX/RG-DS, PulseAudio failed before opening an audio device. The
failure-triggered retry opened
`rk817_ext, fe410000.i2s-rk817-hifi rk817-hifi-0` through ALSA at 32 kHz,
stereo S16LE with both buffers queued. The physical run completed 2,197 audio
callbacks, one startup underrun representing 4,096 missing bytes, zero audio
failures and a clean shutdown after 1,550 gameplay frames; the owner reported
clear audio.

The same clean ROCKNIX/Wayland installation did not present the NXExtract
progress UI: the display remained black while more than 1 GiB was prepared.
The log nevertheless recorded complete validation and transactional
publication before gameplay started. This is a display limitation during the
first preparation, not an extraction failure; leave the device running and
follow `debug.log` when visual progress is absent.

Across those runs:

- original first-run dialogs completed in sequence, followed by the controller
  title screen and entry into the first level;
- the first rooftop/city scene, Spider-Man, enemies, HUD, minimap and pause
  menu rendered correctly;
- the controller bridge's A event advanced the title flow, its left-stick axis
  changed world position, its right-stick axis rotated the camera, R2 advanced
  the action tutorial, and Start opened the original in-game pause menu;
- the OpenSL ES bridge opened the game's requested 32 kHz stereo S16LE stream
  through SDL2;
- the first session created a checkpoint and completed a clean shutdown after
  48,400 frames;
- a second session with the same profile skipped the first-run terms, update
  and cloud notices, returned to the menu, reloaded the saved mission and ran
  another 9,777 frames; and
- the stripped baseline binary passed launcher-driven fresh-profile and
  existing-profile runs through title, menu, mission and tutorial, then exited
  cleanly through the launcher in both cases.

Release R2 also passes the local starvation, `Clear`, restart,
teardown-ownership, sanitizer and 50,000-object registry regressions. An
extended target run through mission 2 verified stable audio, the functional
item-and-suit shop and standard Xbox controls on the same build.

### Bring your own game data — required

This repository contains only the loader source and launcher. It does **not**
ship Gameloft's APK, `libtasm2.so`, OBB files, assets or other copyrighted game
content. You must own and supply one exact supported Android input set.
NXExtract identifies content rather than trusting external filenames.

| Android container | SHA-256 | Runtime scope |
|---|---|---|
| 1.2.7d APK, recovery ZIP layout | `4188a463432b921dfb767a3ddf316e970655789a7bdf806298757f45071a8c87` | ARMv7 + x86 |
| 1.2.7d APK, standard ZIP layout | `2878fec3235a91a0487ee0a3ffdbcb5c534e0d052a573941a10489024b2b1868` | ARMv7 + x86 |
| 1.2.8d APK | `6211d194cb06c6cbb32c2491adef59554eef4d97763a2fbc1e4bbb52d9fcae9b` | ARMv7 + x86 |
| 1.2.8d self-contained installer | `42d1a3ac86708549fb425b8e36338ece56ea384fb2e30062c7a7da6ca34689e3` | ARM32/multilib only |

For a loose APK, also supply either the intact companion cache ZIP
`23f3ef198f731fa1af0f2dfce4902e510dc4bf57edc3a3524e8986b2a4bcc770`
or the two required OBBs below. The self-contained installer already carries
those expansions.

| Expansion | Requirement | SHA-256 |
|---|---|---|
| `main.12032.com.gameloft.android.ANMP.GloftASHM.obb` | required | `276c413051b3349e7738afb23521f972d085a186cb22ab18db230906aab46981` |
| `patch.12723.com.gameloft.android.ANMP.GloftASHM.obb` | required | `0faae1e92ab998b8808e3984e4cdafbe732a87c26da58a44ad11c633e643cb80` |
| `patch.12438.com.gameloft.android.ANMP.GloftASHM.obb` | optional | `58d9ed565ad67ee7362a2376a74387316535975460110eccf3df7eb3b6503981` |

All accepted containers produce the exact ARMv7 library
`d091fe95c56af681f1a06453e9868622935ecbb759c05c340fc16bf8df2ae62e`.
The three universal profiles also produce the exact x86 library
`d146d38574c19a105df8a46e523f626c06004c8f71bbeed5cf77e919dbf81a12`.
The self-contained installer has no x86 library and is rejected clearly on the
X5M/Box32 route.

Put one supported set in `ports/asm2_127/gamedata/` and launch the port.
NXExtract validates every source and output, normalizes the recovery-layout
APK when needed, and publishes the prepared runtime transactionally. Do not
unpack or modify OBBs or the cache ZIP.

The launcher belongs in the ports root; the executable, game library and all
BYO data stay in the `asm2_127/` game directory:

```text
/storage/roms/ports/
├── ASM2-1.2.7d.sh
└── asm2_127/
    ├── asm2_127
    ├── libtasm2.so
    ├── assets/
    │   ├── IapLocalData_Google.json
    │   └── IapStoreItems_Offline.json
    └── gamefiles/
        ├── base.apk
        └── Android/
            ├── data/com.gameloft.android.ANMP.GloftASHM/
            │   ├── cache/                     # created at runtime
            │   └── files/save/                # saves + DataSharing store
            └── obb/com.gameloft.android.ANMP.GloftASHM/
                ├── main.12032.com.gameloft.android.ANMP.GloftASHM.obb
                ├── patch.12723.com.gameloft.android.ANMP.GloftASHM.obb
                └── patch.12438.com.gameloft.android.ANMP.GloftASHM.obb # optional
```

Do not commit or redistribute the staging tree. `.gitignore` excludes the
normal proprietary outputs (`gamefiles/`, APK, OBB and shared libraries).

### Build and run

Public ARMHF releases use the Debian Buster recipe and require at most
GLIBC 2.30. It builds a stripped, dynamically linked ELF32 ARM executable
(`Type: EXEC`, EABI5 hard-float) for both the NextOS and PortMaster package
routes:

```bash
cd ports/asm2_127
TARGET_HEADER_SYSROOT=/path/to/header-sysroot
docker run --rm -v "$PWD":/repo -v "$TARGET_HEADER_SYSROOT":/sysroot:ro \
  debian:buster bash /repo/build_buster_arkos.sh
```

The X5M route uses the separate low-glibc i386 loader plus the source-built
AArch64 Box32 host and `sdl2-compat`. The release scripts audit each output
against the same GLIBC 2.30 ceiling:

```bash
docker run --rm -v "$PWD":/repo -v "$TARGET_HEADER_SYSROOT":/sysroot:ro \
  debian:buster bash /repo/build_buster_x86_box32.sh
../release-tools/build-box32-x5m.sh SOURCE BUILD
../release-tools/build-sdl2-compat-x5m.sh SOURCE BUILD
```

`build.sh` and `build_x86_box32.sh` remain developer recipes for explicitly
current-sysroot, non-universal diagnostics; their outputs are not accepted as
the sole binaries in the public package.

Install `asm2_127-staging/` as `/storage/roms/ports/asm2_127/` and install
`ASM2-1.2.7d.sh` one level above it. Run the launcher in the foreground so the
frontend can resume when the game exits:

```bash
cd /storage/roms/ports
./ASM2-1.2.7d.sh
```

For an EmulationStation entry, install `The Amazing Spider-Man 2.sh` in
`/storage/roms/ports_scripts/`. The wrapper keeps the same foreground launcher
path used by hardware validation.

The launcher sets the 32-bit library path and `ASM2_RUN=1`. It does not force an
SDL video driver, allowing the system's Mali fbdev/EGL stack to be selected.

| Environment variable | Effect |
|---|---|
| `ASM2_RUN=1` | enters the continuous game/input/render loop; set by the launcher |
| `ASM2_FORCE_OPAQUE=1` | forces backbuffer alpha to 1 before swap for compositors that use framebuffer pixel alpha; off by default |

### Source map

- `src/main.c` — ELF/JNI bootstrap, native init order, lifecycle, frame loop,
  concise crash/support summary and clean shutdown.
- `src/so_util.c`, `src/so_util_x86.c`, `src/so_util.h` — independent ARM32
  and i386 ELF mapping, relocation, symbol lookup and init arrays.
- `src/x86_runtime_compat.c`, `src/x86_gl_lazy.S` — Box32-safe destructor,
  program-header and lazy GL bridges with measured i386 ABI alignment.
- `src/capture_x86.c` — deterministic backbuffer capture used for physical
  X5M evidence.
- `src/imports.c` — explicit compatibility import table.
- `src/bionic_compat.c` — Bionic libc data/layout bridges, 84-byte `FILE`
  wrappers, Android path virtualization and filesystem-structure conversion.
- `src/pthread_bridge.c` — pthread, condition, TLS, once and semaphore object
  bridge.
- `src/softfp_bridge.c`, `src/setjmp_bridge.S` — base-PCS/hard-float and Bionic
  register-layout bridges.
- `src/jni_bridge.c` — self-contained JNI 1.6 VM, objects, strings, arrays,
  fields, method dispatch and `RegisterNatives` registry.
- `src/android_callbacks.c` — Activity/device/platform callbacks, app paths,
  exit requests and durable `DataSharing` store.
- `src/shop_compat.c` — deterministic offline item/suit catalog and original
  shop-state progression without a live service.
- `src/installer_compat.c` — layout-checked postcondition of the 1.2.7d Android
  installer phase.
- `src/startup_compat.c` — version-checked serialization of the two original
  first-run dialogs, restoring the guest's own call after the first closes.
- `src/video.c` — fullscreen SDL/EGL/GLES2 setup and optional opaque-alpha swap.
- `src/input.c` — SDL controller hotplug, HID axes/buttons, keyboard, mouse and
  multitouch forwarding.
- `src/opensl_bridge.c` — OpenSL ES buffer queue over SDL2 audio.
- `src/audio_compat.c` — version-checked 1.2.7d audio-callback mutex handoff.
- `src/platform_shims.c` — GLES extension lookup, NDK sensor/looper shims and
  ARM unwind lookup.
- `tools/extract_apk.py` — CRC-checked recovery of selected APK members.
- `tools/rebuild_runtime_apk.py` — atomic construction and full verification of
  a standards-compliant runtime APK.
- `tools/extract_shop_assets.py` — exact-version extraction of the catalog
  embedded in the main OBB and deterministic generation of its offline IAB
  response; both outputs remain BYO data outside Git.
- `x5m-runtime-env.sh` — public process-scope boundary: firmware SDL2/KMSDRM
  for native NXExtract and packaged `sdl2-compat` only for the Box32 game.
- `ASM2-1.2.7d.sh` — NextOS launcher, installed beside the `asm2_127/`
  directory in the ports root.
- `The Amazing Spider-Man 2.sh` — minimal EmulationStation wrapper installed
  in `ports_scripts/`; it executes the tested launcher in foreground.
- `CLEANROOM.md` — allowed-input and provenance boundary.
- `tests/` — starvation/teardown, FILE, JNI, synchronization and persistent
  shared-value regressions.

### Sources, licenses and ownership

- Port glue, scripts and repository-owned loader changes are distributed under
  the repository's [GNU GPL v3 license](../../LICENSE), unless an individual
  file states otherwise.
- The generic `so_util` loader retains its in-file attribution to Jaakko
  Lukkari, fgsfds and Andy Nguyen; applicable upstream notices remain in force.
- SDL2, EGL, GLES2, glibc and the cross-toolchain are external dependencies and
  remain under their respective licenses.
- The Amazing Spider-Man 2, the APK, `libtasm2.so`, OBB files, assets,
  trademarks and all other game content remain proprietary to their respective
  owners. They are not licensed by this repository and must not be included in
  a source or binary release of the port.

This project is an independent compatibility effort and is not affiliated with
or endorsed by the game's rights holders.

---

## Português

Este é um port independente de compatibilidade de The Amazing Spider-Man 2. Um
loader ELF próprio executa a `libtasm2.so` original diretamente em ARMHF, sem
userspace Android nem máquina virtual Java. Na rota X5M AArch64 validada, um
loader i386 separado executa a `libtasm2-x86.so` original por Box32 e
`sdl2-compat`. As duas rotas fornecem somente as superfícies Bionic, JNI, ciclo
de vida, armazenamento, áudio, vídeo e entrada exigidas pelo binário exato.

Estado: **JOGÁVEL em todas as rotas validadas fisicamente.** Gameplay,
controles, áudio e encerramento limpo foram validados no NextOS/Mali-450,
ArkOS/R36S, muOS/RG 40XX-H, ROCKNIX/RG-DS e NextOS/X5M; a matriz original
também passou criação de checkpoint e recarga do save. Partindo de dados novos,
o jogo segue termos legais, aviso de atualização, aviso nativo de dados na
nuvem, controles, carregamento progressivo e primeira fase. Esse aviso não
exige download e deixa de aparecer depois que o perfil conclui o fluxo inicial.

A versão 1.1.8 substitui a conversão legal única por uma recuperação persistida
em três etapas: toque ajustado ao drawable nos termos, HID nativo no log de
atualização e toque centralizado em qualquer variante do aviso de nuvem. O
layout 720×720 usa a linha do botão ACCEPT medida na imagem enviada. Marcadores
antigos são migrados por classe de drawable e somente o `ud_Control.sav` criado
pelo jogo torna o fluxo permanentemente concluído. A sequência completa e o
reinício sem interceptação passaram fisicamente no ArkOS em 640×480. A etapa do
log normaliza qualquer botão frontal para o A lógico do jogo, inclusive em
controles Nintendo cujas etiquetas A/B diferem do mapeamento SDL.

A primeira sessão de validação criou um checkpoint e encerrou de forma limpa
após 48.400 frames. O reinício com o mesmo perfil pulou termos, atualização e
aviso de nuvem, voltou ao menu, recarregou a missão e rodou mais 9.777 frames.
Depois, o binário-base stripado passou testes conduzidos pelo launcher tanto
com perfil novo quanto com perfil existente, cobrindo título, menu, missão,
tutorial e saída limpa de volta pelo launcher.

O release **R2** mantém essas proteções e foi validado no aparelho até a missão
2. O áudio permaneceu estável na transição e durante a sessão longa, a loja
offline de itens e trajes abriu e continuou navegável, e um controle Xbox
padrão operou gameplay, câmera e os menus de título, opções, pausa e loja.

### Limite clean-room

A implementação foi produzida somente a partir de:

- contêineres Android 1.2.7d/1.2.8d auditados e expansões correspondentes
  pertencentes ao usuário;
- componentes genéricos e independentes de jogo já presentes neste repositório;
- documentação pública de APIs/plataformas; e
- lições conceituais de ports concluídos e funcionais.

Nenhuma implementação anterior deste port foi usada como fonte para este
diretório. Offsets e comportamentos específicos são conferidos contra o binário
1.2.7d exato compartilhado por esses contêineres e falham de forma segura se o
layout não corresponder. Consulte
[`CLEANROOM.md`](CLEANROOM.md).

### Arquitetura

1. `so_util.c` mapeia o ELF ARM32 e `so_util_x86.c` mapeia separadamente o ELF
   i386. Ambos aplicam relocações Android `REL`, resolvem imports e executam os
   construtores do guest.
2. `imports.c` encaminha símbolos sensíveis à ABI por bridges explícitos.
   Funções simples de libc/GLES2 com argumentos inteiros ou ponteiros podem cair
   no host.
3. `bionic_compat.c`, `pthread_bridge.c`, `softfp_bridge.c` e
   `setjmp_bridge.S` traduzem layouts e convenções de chamada do Bionic ARM32
   antigo para o ambiente glibc/hard-float do NextOS.
4. `jni_bridge.c` fornece uma VM/ambiente JNI 1.6 autocontido. Os callbacks
   Android reproduzem apenas o comportamento de Activity, aparelho, paths,
   preferências e `DataSharing` necessário à versão 1.2.7d.
5. `main.c` dirige os entry points JNI originais na ordem do ciclo de vida
   Android e depois assume o loop SDL de eventos/renderização.
6. `video.c` cria um contexto SDL/EGL/GLES fullscreen; `input.c` encaminha
   eventos HID do controle, keycodes Android e toque. No X5M, as funções GL do
   guest são resolvidas pelo SDL depois que o contexto real existe.
7. `opensl_bridge.c` implementa engine, output mix, player e fila de buffers do
   OpenSL ES sobre áudio SDL2. O PCM S16LE pedido pelo jogo é enfileirado sem
   chamar o guest enquanto o mutex de áudio está travado.
8. Os paths de armazenamento externo Android são virtualizados dentro de
   `gamefiles/`, mantendo dados originais, saves e cache junto do port.

O release usa de forma determinística o caminho offline nativo do próprio jogo.
A transação de perfil EVE da Gameloft de 2014 não conclui: com um link de rede
ativo, o guest repete em 45% mesmo quando a raiz do serviço responde. Por isso,
o callback de plataforma informa ausência de conexão e a máquina de estados
original segue seu branch offline completo e embutido.

Anúncios, navegador, notificações e integrações sociais não são recriados.
Seus callbacks Android retornam valores offline seguros ou não fazem nada
quando apropriado.

### Barreiras vencidas

| Barreira | Causa | Solução |
|---|---|---|
| leitores ZIP comuns rejeitam o APK | offsets deslocados no diretório central e um byte raw-DEFLATE ausente no insumo 1.2.7d conhecido | recuperar pelos headers locais físicos, validar tamanho/CRC e gerar um APK de runtime separado e normalizado |
| crash intermitente no ZIP e acesso progressivamente mais lento | a libzip antiga lê campos de um `FILE` Bionic de 84 bytes, enquanto a glibc expõe outro objeto opaco; um registro histórico linear encarecia cada busca | wrappers guest sincronizados em hash ativo, mantendo tombstones obsoletos fora do caminho quente |
| corrupção de mutex/condition/semaphore, timeouts travados e custo crescente de busca | objetos do Bionic antigo são menores que os da glibc, o jogo cria deadlines absolutos com `gettimeofday` e objetos aposentados permaneciam nos buckets ativos | objetos pthread/sem reais, waits de condition no relógio realtime compatível com Bionic, buckets somente com ativos e tombstones aposentados separados |
| argumentos/retornos float incorretos | o guest usa AAPCS base e o toolchain NextOS usa ARM hard-float | bridges explícitos softfp→hardfp para matemática e GLES |
| bootstrap Java/Activity ausente | o jogo espera JNI 1.6, estado do instalador e uma sequência exata de init nativo | VM/ambiente falsos, dispatch de natives registrados, pós-condição do instalador travada na versão e driver de lifecycle ordenado |
| crash ao conectar o controle no início | o listener alcança objetos da engine criados somente no primeiro frame GL | renderizar um frame de aquecimento antes de abrir o controle SDL |
| áudio pode permanecer mudo após uma transição | o stream original de dois buffers pode cair num estado vazio absorvente quando um callback perde a janela de refill | recuperação temporizada de um refill por vez, buffers PCM reutilizáveis e worker em prioridade alta best-effort |
| teardown de áudio pode travar ou sobreviver ao contexto guest | o destrutor 1.2.7d segura o mutex do mixer durante `Destroy`, enquanto o callback precisa do mesmo mutex | handoff de mutex conferido por versão, join externo síncrono e cleanup pelo worker somente na autodestruição |
| gameplay ARMHF fica sem áudio no ROCKNIX | a SDL de 32 bits herda PulseAudio, mas esse servidor não está acessível ao processo ARMHF enquanto a rota ALSA RK817 do firmware funciona | somente no build externo de glibc baixa, preservar primeiro a escolha herdada e tentar ALSA uma vez após falha da inicialização automática ou do PulseAudio herdado |
| paths Android inexistentes | a engine usa `/sdcard`, dados privados, cache, APK e OBB | mapear os prefixos Android para a árvore local `gamefiles/` |
| preferências do primeiro início somem ao sair | no Android o `DataSharing` persiste via Java | store binário limitado e serializado por mutex, CRC, commit atômico por temporário/rename, supressão de valor idêntico e rollback em falha |
| os dois diálogos iniciais não podem ser fechados | o aviso offline de dados e `UI_FIRST_CHECK` usam o mesmo `ConfirmBoxSYS` no mesmo frame, então o callback posterior substitui o callback do diálogo visível | adiar somente o segundo builder, restaurar a instrução original conferida por versão e deixar o jogo criá-lo no frame seguinte |
| o ACCEPT legal não reage em telas quadradas e avisos de nuvem posteriores também podem prender o input | a conversão antiga reutilizava a coordenada Y de 4:3 em 720×720 e marcava a tentativa como permanente antes de o jogo comprovar a conclusão | usar coordenadas legais/nuvem por layout, HID nativo no log, migrar marcadores antigos e aceitar apenas o perfil de controles do jogo como prova permanente |
| rede ativa trava em 45% | a transação de perfil EVE legada não conclui mesmo com a raiz do endpoint acessível | informar ausência de conexão de forma determinística para o guest seguir seu branch offline nativo completo |
| teardown GL inseguro após sessão ativa | a engine percorre objetos de vida igual à do processo já obsoletos em `GL2JNILib_destroy` | executar pause/saída de opções/input e deixar a saída normal do processo recuperar os globais do guest |
| entry points de extensões ausentes no link | o userspace Mali expõe certas extensões GLES apenas pelo resolvedor em runtime | busca de extensões e shims compatíveis com GLES2 |
| chamadas GL i386 falham de forma intermitente no dynarec padrão | o primeiro bridge GL entrava em C com stack i386 desalinhada e o driver X5M exige tratamento mais conservador dos blocos/flags | preservar o ABI SysV i386 de 16 bytes e restringir `DYNAREC=1`, `BIGBLOCK=0`, `SAFEFLAGS=2` à rota X5M exata |
| o X5M AArch64 não possui userspace ARMHF | não há como executar o loader ou a biblioteca ARMv7 nativamente | usar a biblioteca Android x86 genuína do APK por Box32 compilado de fonte e `sdl2-compat` AArch64, sem mudar o lifecycle Android |
| o extrator da primeira execução fica preto no X5M | a interface AArch64 nativa herdava o `sdl2-compat` exclusivo do jogo, que não consegue receber o handoff KMS do EmulationStation antes do Box32 | usar SDL2/KMSDRM do firmware no NXExtract e restringir o `sdl2-compat` empacotado à execução Box32; o teste de contrato do pacote rejeita qualquer vazamento entre os dois caminhos |

### Áudio

A biblioteca Android importa interfaces de objeto, engine, play e fila de
buffers no estilo OpenSL ES 1.0. O bridge copia o PCM para buffers reutilizáveis
do host, o callback SDL os consome e um worker separado chama o callback de
conclusão. Uma fila seca é consultada de forma conservadora e agenda no máximo
um callback de recuperação por vez, tentando novamente a cada 50 ms até o guest
reenfileirar. O callback SDL de tempo real não aloca memória nem entra no guest.

O destrutor e o callback de conclusão exatos da 1.2.7d usam o mesmo mutex guest.
Suas instruções e o prólogo do callback são conferidos antes da execução. Se o
teardown encontra um callback em voo, o bridge pthread entrega temporariamente
o mutex ao callback, espera o worker terminar e o readquire antes de `Destroy`
retornar. Assim preserva a exclusão original sem espera circular nem acesso a
um contexto já liberado.

Relatórios OpenSL a cada cinco segundos mostram fila, callbacks secos/de
recuperação, underruns, bytes ausentes, enqueues obsoletos e pico PCM. Relatórios
de runtime a cada quinze segundos mostram memória/swap, crescimento dos
registros, alocações JNI e fallbacks de fences gráficos sem varrer o heap.

O launcher detecta um socket PulseAudio disponível e, caso não encontre, deixa
o SDL escolher o backend e a saída de áudio padrão do sistema. Somente no build
ARMHF externo de glibc baixa, o bridge OpenSL tenta ALSA uma vez se a
inicialização automática ou do PulseAudio herdado falhar. Escolhas explícitas
de diagnóstico permanecem intactas, e os binários atuais ARMHF NextOS e X5M
não ativam esse fallback.

### Controles

Os controles Xbox padrão foram validados no aparelho. O jogo recebe seus
eventos Android originais; não é necessário cursor durante o gameplay nem
mapeador de teclas do desktop.

Os eventos de controle usam os identificadores originais de
`NativeBridgeHIDControllers`. A tela oficial de controle atribui as principais
ações abaixo; menus e sequências guiadas podem reutilizar os mesmos botões.

| Entrada física | Entrada enviada ao guest / ação oficial |
|---|---|
| Analógico esquerdo | movimento, com dead zone de 12% |
| Analógico direito | câmera, com dead zone de 12% |
| R2 | gatilho direito contínuo; pressionar para pular, segurar para se balançar |
| L2 | gatilho esquerdo contínuo; combo especial |
| X | atacar |
| B | disparar teia |
| Y | esquivar |
| A | próximo diálogo |
| L1 / R1 | shoulder esquerdo/direito; ação especial |
| Direcional | botões HID cima, baixo, esquerda e direita |
| Clique dos analógicos | botão do analógico esquerdo/direito |
| Start / Select / Guide | Start, Select e Back; Start abre a pausa |
| Select + Start | encerra o port de forma limpa |
| Tela de toque | até dez ponteiros nativos |
| Botão esquerdo do mouse | toque/arrasto/soltura, útil em menus feitos para touch |

Fallback de teclado:

| Teclas | Entrada enviada ao guest |
|---|---|
| Setas | Direcional |
| `Z` / `X` / `A` / `S` | A / B / X / Y |
| `Q` / `W` | L1 / R1 |
| `1` / `2` | L2 / R2 |
| Shift esquerdo/direito | clique do analógico esquerdo/direito |
| Espaço / Tab / Home | Start / Select / Guide |
| Enter | Enter Android |
| Escape ou Backspace | Back Android |

### Validação física de release

A linha de base da validação física de gameplay cobre:

- NextOS/Mali-450 com o mesmo fonte de runtime ARMHF preservado aqui;
- ArkOS/R36S com o loader ARMHF público exato da v1.1.8;
- muOS/RG 40XX-H com os módulos ALSA, PipeWire e SPA de 32 bits do firmware;
- ROCKNIX/RG-DS com Panfrost, Wayland, Mesa 26.1.2 e fallback ALSA restrito; e
- NextOS/X5M/Mali-G310 com loader i386 alinhado, Box32 e KMSDRM/GLES.

No X5M, a validação final completou 11.034 frames de gameplay e 6.213 frames
após reabrir, ambos com saída zero. O save foi criado, atualizado e recarregado,
o áudio não teve falhas de callback e o controle físico funcionou. Os alvos
ARM também concluíram o fluxo inicial nativo, carregamento progressivo,
gameplay e recarga do save.

Na v1.1.8, os binários X5M foram recompilados duas vezes de forma idêntica
abaixo do teto GLIBC 2.30. O mesmo fonte/patch/perfil seguro do host Box32 inicia
sob QEMU AArch64 e o loader i386 recompilado chega ao limite esperado de
biblioteca proprietária ausente; esses bytes X5M exatos não foram executados
novamente na placa física.

No ROCKNIX/RG-DS, o PulseAudio falhou antes de abrir um dispositivo. A nova
tentativa abriu
`rk817_ext, fe410000.i2s-rk817-hifi rk817-hifi-0` por ALSA em 32 kHz,
estéreo S16LE, com os dois buffers enfileirados. A execução física completou
2.197 callbacks, um underrun inicial correspondente a 4.096 bytes ausentes,
zero falhas de áudio e encerramento limpo após 1.550 frames; o dono informou
áudio claro.

Nessa mesma instalação limpa no ROCKNIX/Wayland, a interface de progresso do
NXExtract não apareceu: a tela permaneceu preta enquanto mais de 1 GiB era
preparado. O log registrou validação completa e publicação transacional antes
do gameplay. Isso é uma limitação visual da primeira preparação, não uma falha
de extração; deixe o aparelho ligado e acompanhe `debug.log` quando não houver
progresso visível.

Em conjunto, os testes observaram:

- os diálogos originais do primeiro início foram concluídos em sequência,
  seguidos pela tela de controle e pela entrada na primeira fase;
- a primeira cena de telhado/cidade, Homem-Aranha, inimigos, HUD, minimapa e
  menu de pausa renderizaram corretamente;
- o evento A do bridge de controle avançou a tela de título, o eixo do analógico
  esquerdo mudou a posição no mundo, o eixo do analógico direito girou a
  câmera, R2 avançou o tutorial de ação e Start abriu o menu de pausa original;
- o bridge OpenSL ES abriu via SDL2 o stream S16LE estéreo a 32 kHz pedido pelo
  jogo;
- a primeira sessão criou um checkpoint e concluiu o encerramento limpo após
  48.400 frames;
- uma segunda sessão com o mesmo perfil pulou os avisos iniciais de termos,
  atualização e nuvem, voltou ao menu, recarregou a missão salva e rodou mais
  9.777 frames; e
- o binário-base stripado passou execuções conduzidas pelo launcher com
  perfil novo e perfil existente, atravessou título, menu, missão e tutorial e
  encerrou de forma limpa pelo launcher nos dois casos.

O release R2 também passa as regressões locais de starvation, `Clear`, retomada,
ownership de teardown, sanitizers e registros com 50.000 objetos. Uma execução
longa no aparelho até a missão 2 confirmou no mesmo build o áudio estável, a
loja funcional de itens e trajes e os controles Xbox padrão.

### Forneça seus próprios dados — obrigatório

Este repositório contém somente o código do loader e o launcher. Ele **não**
distribui o APK da Gameloft, `libtasm2.so`, OBBs, assets ou qualquer outro
conteúdo protegido do jogo. Você precisa possuir e fornecer um conjunto
Android exato e suportado. O NXExtract identifica o conteúdo, não o nome
externo do arquivo.

| Contêiner Android | SHA-256 | Escopo de runtime |
|---|---|---|
| APK 1.2.7d, layout ZIP de recuperação | `4188a463432b921dfb767a3ddf316e970655789a7bdf806298757f45071a8c87` | ARMv7 + x86 |
| APK 1.2.7d, layout ZIP normal | `2878fec3235a91a0487ee0a3ffdbcb5c534e0d052a573941a10489024b2b1868` | ARMv7 + x86 |
| APK 1.2.8d | `6211d194cb06c6cbb32c2491adef59554eef4d97763a2fbc1e4bbb52d9fcae9b` | ARMv7 + x86 |
| instalador 1.2.8d autocontido | `42d1a3ac86708549fb425b8e36338ece56ea384fb2e30062c7a7da6ca34689e3` | somente ARM32/multilib |

Para um APK solto, forneça também o cache ZIP intacto
`23f3ef198f731fa1af0f2dfce4902e510dc4bf57edc3a3524e8986b2a4bcc770`
ou os dois OBBs obrigatórios abaixo. O instalador autocontido já traz essas
expansões.

| Expansão | Exigência | SHA-256 |
|---|---|---|
| `main.12032.com.gameloft.android.ANMP.GloftASHM.obb` | obrigatória | `276c413051b3349e7738afb23521f972d085a186cb22ab18db230906aab46981` |
| `patch.12723.com.gameloft.android.ANMP.GloftASHM.obb` | obrigatória | `0faae1e92ab998b8808e3984e4cdafbe732a87c26da58a44ad11c633e643cb80` |
| `patch.12438.com.gameloft.android.ANMP.GloftASHM.obb` | opcional | `58d9ed565ad67ee7362a2376a74387316535975460110eccf3df7eb3b6503981` |

Todos os contêineres aceitos produzem a biblioteca ARMv7 exata
`d091fe95c56af681f1a06453e9868622935ecbb759c05c340fc16bf8df2ae62e`.
Os três perfis universais também produzem a biblioteca x86 exata
`d146d38574c19a105df8a46e523f626c06004c8f71bbeed5cf77e919dbf81a12`.
O instalador autocontido não possui biblioteca x86 e é recusado de forma clara
na rota X5M/Box32.

Coloque um conjunto suportado em `ports/asm2_127/gamedata/` e abra o port. O
NXExtract valida cada fonte e saída, normaliza o APK com layout de recuperação
quando necessário e publica o runtime preparado de forma transacional. Não
abra, modifique nem extraia OBBs ou o cache ZIP.

O launcher fica na raiz de ports; o executável, a biblioteca do jogo e todos os
dados BYO permanecem no diretório de jogo `asm2_127/`:

```text
/storage/roms/ports/
├── ASM2-1.2.7d.sh
└── asm2_127/
    ├── asm2_127
    ├── libtasm2.so
    ├── assets/
    │   ├── IapLocalData_Google.json
    │   └── IapStoreItems_Offline.json
    └── gamefiles/
        ├── base.apk
        └── Android/
            ├── data/com.gameloft.android.ANMP.GloftASHM/
            │   ├── cache/                     # criado em runtime
            │   └── files/save/                # saves + store DataSharing
            └── obb/com.gameloft.android.ANMP.GloftASHM/
                ├── main.12032.com.gameloft.android.ANMP.GloftASHM.obb
                ├── patch.12723.com.gameloft.android.ANMP.GloftASHM.obb
                └── patch.12438.com.gameloft.android.ANMP.GloftASHM.obb # opcional
```

Não faça commit nem redistribua o staging. O `.gitignore` exclui as saídas
proprietárias normais (`gamefiles/`, APK, OBB e bibliotecas compartilhadas).

### Compilar e rodar

Releases ARMHF públicas usam a receita Debian Buster e exigem no máximo
GLIBC 2.30. Ela produz um ELF32 ARM stripado (`Type: EXEC`, EABI5 hard-float)
para as rotas NextOS e PortMaster do pacote:

```bash
cd ports/asm2_127
TARGET_HEADER_SYSROOT=/caminho/do/sysroot-de-headers
docker run --rm -v "$PWD":/repo -v "$TARGET_HEADER_SYSROOT":/sysroot:ro \
  debian:buster bash /repo/build_buster_arkos.sh
```

A rota X5M usa o loader i386 de glibc baixa, o Box32 AArch64 compilado de fonte
e o `sdl2-compat`. Os scripts de release auditam todos contra o mesmo teto
GLIBC 2.30:

```bash
docker run --rm -v "$PWD":/repo -v "$TARGET_HEADER_SYSROOT":/sysroot:ro \
  debian:buster bash /repo/build_buster_x86_box32.sh
../release-tools/build-box32-x5m.sh SOURCE BUILD
../release-tools/build-sdl2-compat-x5m.sh SOURCE BUILD
```

`build.sh` e `build_x86_box32.sh` permanecem receitas de desenvolvimento para
diagnóstico em um sysroot atual e explicitamente não universal; essas saídas
não são aceitas como os únicos binários do pacote público.

Instale `asm2_127-staging/` como `/storage/roms/ports/asm2_127/` e instale
`ASM2-1.2.7d.sh` um nível acima. Execute o launcher em foreground para que o
frontend possa retornar quando o jogo encerrar:

```bash
cd /storage/roms/ports
./ASM2-1.2.7d.sh
```

Para criar a entrada no EmulationStation, instale
`The Amazing Spider-Man 2.sh` em `/storage/roms/ports_scripts/`. O wrapper
mantém o mesmo caminho em foreground usado na validação no aparelho.

O launcher configura o path de bibliotecas 32-bit e `ASM2_RUN=1`. Ele não força
um driver de vídeo SDL, permitindo que o sistema selecione a pilha Mali
fbdev/EGL.

| Variável de ambiente | Efeito |
|---|---|
| `ASM2_RUN=1` | entra no loop contínuo de jogo/input/render; definida pelo launcher |
| `ASM2_FORCE_OPAQUE=1` | força alpha 1 no backbuffer antes do swap em compositores que usam o alpha por pixel do framebuffer; desligada por padrão |

### Mapa dos fontes

- `src/main.c` — bootstrap ELF/JNI, ordem do init nativo, lifecycle, loop de
  frames, resumo conciso de crash para suporte e desligamento limpo.
- `src/so_util.c`, `src/so_util_x86.c`, `src/so_util.h` — mapeamento ELF ARM32
  e i386 independentes, relocações, busca de símbolos e init arrays.
- `src/x86_runtime_compat.c`, `src/x86_gl_lazy.S` — bridges Box32 de
  destrutores, program headers e GL lazy com alinhamento i386 medido.
- `src/capture_x86.c` — captura determinística do backbuffer usada como
  evidência física do X5M.
- `src/imports.c` — tabela explícita de imports de compatibilidade.
- `src/bionic_compat.c` — bridges de dados/layouts libc Bionic, wrappers de
  `FILE` com 84 bytes, virtualização de paths Android e conversão de structs do
  filesystem.
- `src/pthread_bridge.c` — bridge de pthread, condition, TLS, once e semáforos.
- `src/softfp_bridge.c`, `src/setjmp_bridge.S` — bridges base-PCS/hard-float e
  layout de registradores Bionic.
- `src/jni_bridge.c` — VM JNI 1.6 autocontida, objetos, strings, arrays, campos,
  dispatch de métodos e registro de `RegisterNatives`.
- `src/android_callbacks.c` — callbacks de Activity/aparelho/plataforma, paths
  do app, pedido de saída e store `DataSharing` durável.
- `src/shop_compat.c` — catálogo offline determinístico de itens/trajes e
  progressão do estado original da loja sem serviço ativo.
- `src/installer_compat.c` — pós-condição da fase de instalador Android, com
  layout conferido para a 1.2.7d.
- `src/startup_compat.c` — serialização dos dois diálogos originais do primeiro
  início, conferida por versão e restaurando a chamada do próprio guest quando
  o primeiro fecha.
- `src/video.c` — SDL/EGL/GLES2 fullscreen e swap alpha-opaco opcional.
- `src/input.c` — hotplug de controle SDL, eixos/botões HID, teclado, mouse e
  multitouch.
- `src/opensl_bridge.c` — fila de buffers OpenSL ES sobre áudio SDL2.
- `src/audio_compat.c` — handoff do mutex de callback de áudio conferido para a
  versão 1.2.7d.
- `src/platform_shims.c` — resolução de extensões GLES, shims de sensor/looper
  NDK e busca de unwind ARM.
- `tools/extract_apk.py` — recuperação com CRC dos membros necessários do APK.
- `tools/rebuild_runtime_apk.py` — construção atômica e verificação completa de
  um APK normalizado para runtime.
- `tools/extract_shop_assets.py` — extração conferida para a versão exata do
  catálogo embutido no OBB principal e geração determinística da resposta IAB
  offline; ambas as saídas permanecem como dados BYO fora do Git.
- `x5m-runtime-env.sh` — fronteira pública dos processos: SDL2/KMSDRM do
  firmware para o NXExtract nativo e `sdl2-compat` empacotado apenas no jogo
  Box32.
- `ASM2-1.2.7d.sh` — launcher NextOS, instalado ao lado do diretório
  `asm2_127/` na raiz de ports.
- `The Amazing Spider-Man 2.sh` — wrapper mínimo do EmulationStation instalado
  em `ports_scripts/`; executa o launcher validado em foreground.
- `CLEANROOM.md` — limite de insumos e procedência.
- `tests/` — regressões de starvation/teardown, FILE, JNI, sincronização e
  valores compartilhados persistentes.

### Fontes, licenças e propriedade

- A cola do port, os scripts e as alterações do loader pertencentes ao
  repositório são distribuídos sob a [GNU GPL v3](../../LICENSE), salvo quando
  um arquivo declarar outra licença.
- O loader genérico `so_util` mantém nos próprios arquivos a atribuição a
  Jaakko Lukkari, fgsfds e Andy Nguyen; os avisos upstream aplicáveis continuam
  valendo.
- SDL2, EGL, GLES2, glibc e o cross-toolchain são dependências externas e
  permanecem sob suas respectivas licenças.
- The Amazing Spider-Man 2, o APK, `libtasm2.so`, OBBs, assets, marcas e todo o
  restante do conteúdo do jogo permanecem proprietários de seus respectivos
  titulares. Eles não são licenciados por este repositório e não podem entrar
  num release fonte ou binário do port.

Este projeto é um esforço independente de compatibilidade, sem afiliação nem
endosso dos titulares do jogo.
