# Changelog

All notable changes to this port are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 1.0.1

Release hygiene pass; no gameplay changes.

### Changed

- The whole debug/tuning environment surface was renamed to the port's own
  `COI_*` namespace (`COI_RES`, `COI_GLVER`, `COI_SWAPINT`, `COI_DEBUG`,
  `COI_PAGE*`, audio knobs, log gates), and the `/dev/shm` trigger files moved
  to `coi_*`. The old donor-scaffold names are gone.
- Every runtime message the launcher prints — on the frontend TTY and in
  `debug.log` — is in English now, and `gamedata/LEIA-ME.txt` became a
  bilingual `gamedata/README.txt` (English first).
- The documentation now describes the validation the installer actually
  performs since 1.0.0 — structural (entry name, ELF machine, size range,
  OBB signature) — instead of the retired single-build SHA-256 gate.

### Removed

- Dead donor-scaffold sources that were never compiled into this port
  (`setup_splash.c`, `texbake.c`, `fixpak.c`, `jpeg_enc.c`, `etc1_test.c`,
  `imports.gen.c`), a dormant wrong-package default in the JNI shim, and a
  dead log-redirect branch keyed on a filename this engine never writes.

## 1.0.0

Multi-device packaging of the Mali-450 port. Validated on R36S/ArkOS and on
NextOS Amlogic; awaiting the maintainer's go-ahead to publish.

### Known characteristics

- Gameplay runs at roughly 16–18 fps on a Mali-G31 at 640×480 (menus and
  cutscenes hold 24–30), and the first level's loading is long on that class of
  device. This is the engine/GPU combination, not a regression.
- Cutscenes are skipped with **L1**, which issues a touch at the centre of the
  real drawable. The game's "Tap to Skip" prompt only answers to touch — no
  button reaches it.

### Note on an earlier misdiagnosis

An earlier revision of this file claimed the touch-only title screen made the
game impossible to start with a controller. That was wrong: the menus are
reachable, the loading is simply slow, and the "intro loop" observed on the
bench was impatience plus an instrument — injected input and `kmsgrab` screen
capture competing with the game for the display. Gameplay was reached on real
hardware. `g_cursor_down` is still dead code, but it is not blocking anything.

### Added

- **BYO-data install (NXExtract 1.2.2).** The package ships no game data. The
  engine library, FMOD Ex and the `main.154` OBB are extracted from the user's
  own APK and OBB, selected by content and validated **structurally** — entry
  name, ELF machine and a size range for the libraries, magic and size range for
  the OBB — then published in one transaction with resume, rollback and a fast
  marker for later launches. The user's files are never deleted.
- **Any Play build of 1.4.5 is accepted.** The recipe deliberately does not gate
  on the SHA-256 of one reference build: the store ships several builds and
  respins of the same visible version, and users extract with different tools,
  so a single hash rejects a legitimately purchased copy.
- **PortMaster launcher in two layers.** A thin visible `Castle of Illusion.sh`
  that resolves its own symlink and covers every known ROM layout, and a
  supervising `run.sh` that owns logging, single-instance enforcement, the data
  gate and the run itself. A failure at any point writes a log and prints on
  the frontend console — it never fails silently.
- **One exit path for every source.** `SELECT`+`START` from a mapped pad, from a
  raw pad that SDL does not recognise, from gptokeyb's `ESC`+`ENTER`, and
  `SIGTERM` from the frontend all pause the game, let the engine write its save
  and only then exit. An `alarm()` backstop guarantees the frontend is never
  held hostage by a wedged loop.
- **Raw-pad exit fallback.** On firmware whose pad is not in SDL's built-in
  database the GameController never opens; the exit combo is now also read
  straight from evdev. A node only counts as a pad if it declares `EV_ABS` and a
  button in the gamepad range, which keeps IR receivers and remote-control
  keyboards out. The `SELECT`/`START` pair is whatever the device actually
  exposes, in order of confidence: canonical names, then
  `BTN_TRIGGER_HAPPY1/2` (R36S/RG351 and most handhelds), then `BTN_BASE3/BASE4`
  (generic 12-button USB pads). Guessing by ordinal pointed at buttons that do
  not physically exist.

  This path is the exit hotkey only. Gameplay input has always come from SDL and
  is untouched.

### Changed

- **The OBB path is resolved at runtime, not compiled in.** It used to be a
  hardcoded `/storage/roms/...`, which only existed on one firmware. It now
  comes from `COI_OBB`, then the game directory the launcher actually resolved,
  then the working directory.
- **The real drawable is the authority for resolution.** The window size is a
  request; KMSDRM picks a connector mode, fbdev can pan half of `virtual_size`.
  The size the game reads through `ANativeWindow_getWidth/Height` is now taken
  from `SDL_GL_GetDrawableSize` after the context exists, so viewport and touch
  mapping stay correct on every panel.
- **Save data moved to `userdata/`, away from the BYO input box.** The engine
  used to write into `gamedata/` — the same folder the user drops the APK and
  OBB into, and the folder the documentation says can be emptied after install
  to reclaim card space. Someone would eventually have deleted their own save
  along with it. Input and game data are separate directories now.
- **Per-frame GL tracing is gated behind the debug flag** (`COI_DEBUG=1`
  since 1.0.1). A leftover
  `glGetIntegerv` pinpoint from the stack-smash hunt was writing one log line
  per call, every frame, straight to the SD card: 9440 of ~9500 lines in a 45 s
  run. Removing it from the default path also bought about 1 fps.
- **The `/dev/shm` input injectors are off by default** (`COI_DEBUG_INJECT=1` to
  re-enable). They are bench diagnostics: leaving them on cost three `fopen()`
  calls every six frames forever and left a writable input channel into the
  user's game.
- The public binary is built in a `debian:buster` container and gated at
  `GLIBC <= 2.30`; it currently requires at most **GLIBC 2.27**. Link stubs come
  from a versioned symbol list, so the build no longer depends on a private
  toolchain or a pre-existing binary.

### Fixed (inherited from the Mali-450 work)

- Black Mickey and black doors: the `npot_fix` inherited from the donor
  scaffold forced `CLAMP_TO_EDGE` on every texture, so mirrored/repeated UVs
  sampled the atlas border. Default is off, and mipmaps came back with it.
- FMOD Ex crashing in `ld-linux`: `FMOD_OS_Output_GetDefault` probes the output
  with `dlopen("libOpenSLES.so")` and an immediate `dlclose`. Passing our fake
  handle to glibc's real `dlclose` segfaulted; `my_dlclose` now recognises it.
- Obsolete shader patches removed, so the Mali GP compiler gets the original
  shader.
