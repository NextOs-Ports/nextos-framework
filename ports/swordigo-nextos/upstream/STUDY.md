# STUDY — Swordigo 1.4.12 (com.touchfoo.swordigo)

| | |
|---|---|
| Engine | Caver (TouchFoo) — PowerVR POD/PVRT, Lua 5.1, Boost, protobuf |
| ABI | arm64-v8a `libswordigo.so` (~7MB) |
| GL | **GLES1.1 fixed-function** (`libGLESv1_CM`) |
| Audio | OpenAL Soft SFX + Java `MusicPlayer` → `res/*.mp3` (mpg123) |
| Data | APK `assets/resources/` + `res/*.mp3` (no OBB) |
| Ref | NaGaa95 `swordigo_nx` (same build) |

## Native boot order (mirrored in `main.c`)

`setFilesDir` → `setCacheDir` → `setAssetManager` → `googleSignInCompleted(0)` →
`handleApplicationLaunch` → `MusicPlayer_initMusicPlayer` → `setupNativeInterface` →
`setupApplication` → `setApplicationViewSize` → `applicationDidBecomeActive` →
loop `updateApplication`/`drawApplication`.

## Input

Touch-only engine. Pad synthesizes `handleTouchEvent` hitboxes (1280×720 ref) +
`FWKeyboard` Left/Right/Up(jump). SELECT+START → deadline + `_exit(0)`.

## Device notes (NextOS Mali-450)

Host provides `libGLESv1_CM`, `libopenal.so.1`, `libmpg123`. PVRTC assets decode
to RGBA via the game's built-in fallback when PVRTC is unavailable.

## Known gap (level load)

Menu / title / Achievements / New Game dialog render and accept touch. Entering
a new/saved game hits a parser at `.so+0x5806a8` that compares two words at a
stored return address to fixed opcodes (`0xd2801168` / `0xd4000001`) and aborts
on mismatch — an anti-tamper check that always fails under so-loader. Bypass:
NOP those two `cmp`s at `0x580724` / `0x580738` (see `main.c`). After the patch,
New Game reaches the intro cinematic but can still die (segfault / secondary
abort) before free roam — next focus is that path (exception message / missing
JNI `loadSnapshot` / scene load).
