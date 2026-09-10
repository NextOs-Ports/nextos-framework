# Prizefighters 2 — handoff

Updated 2026-07-30. `STUDY.md` preserves the PairIP investigation and the
failed paths that led to the current design. Device addresses, local source
paths and session commands remain private and never belong in this file.

## Honest state

The native ARM64 port is **playable**, not PairIP-blocked. On the authorised
Mali-450 device it reaches a real fight through the original title, menu and
control-selection flow with correct GLES2 image, live FMOD audio and native
controller gameplay. The build runs without Android, Waydroid or a PC emulator.

Verified:

- Built against the current NextOS GCC 16.1/sysroot glibc 2.43, never the old
  Buster compatibility path.
- Original `DT_INIT_ARRAY`, `JNI_OnLoad`, `UnityPlayer.initJni`, surface,
  resume/focus and `nativeRender` order runs in full.
- Four version-pinned PairIP plaintext windows and the complete hidden PLT map
  are restored from BYO data. Production does not load `libpairipcore.so`.
- The version-specific shader tool adds GLES2 platform-5 variants to the
  original Unity asset; the former magenta/wrong-colour image is fixed.
- FMOD's real mixer produces non-zero signed-16 PCM and the physical PulseAudio
  monitor recorded a non-silent output stream.
- Typed SharedPreferences persist across process restarts; the game creates its
  career/custom-content directories below `home/`.
- SDL GameController, hotplug, multi-pad aggregation, the common `0810:0001`
  mapping, generic mappings and `/dev/input/js*` fallback are implemented.
- A real Unity Input System `Gamepad` receives exact 52-byte
  `StateEvent`/`GamepadState` records. The user reached gameplay and tested
  native movement/buttons.
- SDL's down-positive left-stick Y is converted to Unity's up-positive
  coordinate system.
- A real managed Input System `Mouse` receives exact 54-byte
  `StateEvent`/`MouseState` records. The right stick drives the approved
  pixel-art pointer, and an R3 down/up pair was visually verified to click from
  the main menu into Select League.
- Unity's original `showSoftInput`, `nativeSetInputString`, selection,
  visibility and close callbacks are preserved. A GLES2-safe pixel keyboard is
  operated by D-pad/A or right-stick/R3, with B delete, X case toggle and
  Start/Done.
- The text bridge finds the actually focused managed
  `UnityEngine.UI.InputField`, commits its text after Unity's native close and
  calls the field's own `DeactivateInputField()`. Upper/lowercase rendering and
  first-name, last-name and nickname persistence were verified in the created
  `MY FIGHTER` career profile.
- `SELECT+START` performs the same immediate safe exit pattern as the approved
  GTA ports, avoiding a proprietary Mali teardown deadlock.

Do not create `DONE_PF2` yet. The remaining release proof is a user-confirmed
save/reload cycle plus `SELECT+START` returning to an active
`emustation.service`.

## Next action, in order

1. Let the current production session continue; do not launch a second instance.
2. Let the user play the newly created career and a short fight.
3. Exit with `SELECT+START`, verify the process is gone
   and `emustation.service` is active.
4. Relaunch through the normal frontend entry and load the career save.
5. If all checks pass, record the proof and create `DONE_PF2`. The loader-only
   release archive may then be promoted as final.

The old VM/decryption investigation is historical evidence in `STUDY.md`, not
the next implementation path.

## Debug switches

All off by default; the shipped binary is quiet.

| Variable | Effect |
|---|---|
| `PF2_VERBOSE=1` | loader, relocation and lifecycle trace |
| `PF2_LOGCAT=1` | mirror the game's own `__android_log_*` output |
| `PF2_JNILOG=1` | every JNI call, with signature |
| `PF2_PLTLOG=1` | per-slot PLT resolution |
| `PF2_GLLOG=1` | GL calls and shader sources |
| `PF2_AUDIO_TRACE=1` | trace FMOD calls and signed-16 PCM peaks |
| `PF2_NO_AUDIO=1` | disable the FMOD-to-SDL bridge |
| `PF2_VM_CAPTURE=1` | analysis-only one-shot PairIP/overlay capture |
| `PF2_FRAMES=N` | stop after N frames |

The fault reporter (PC against the module bases, plus x0..x30) is always on: it
costs nothing until something crashes, and a fault inside a module we mapped
ourselves has no symbols and no link map without it.

## Completion criterion

`DONE_PF2` may exist only after proof on the device of a real fight entered
through Menu → Game, with GLES2 image, sound, native Gamepad, save/reload and
`SELECT+START` returning to EmulationStation. Everything except save/reload and
the final frontend-return check has now been demonstrated; text-entry
persistence and career creation are also proven.

## Release rules

- NextOS: current toolchain/sysroot, glibc 2.43 today and its successor when
  NextOS updates.
- R36S/glibc 2.30 is not part of this work; it would be a separate variant, and
  only if asked for.
- BYO-data is absolute: no APK, SO, metadata, asset or binary in Git.
- `make dist` must remain loader-only; overlays are user-derived BYO data too.
