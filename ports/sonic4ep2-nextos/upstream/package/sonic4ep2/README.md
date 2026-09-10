# Sonic The Hedgehog 4: Episode II - NextOS V6

This is an AArch64 PortMaster compatibility package for Android release
3.0.0-109 of Sonic The Hedgehog 4: Episode II. It contains the NextOS loader,
launcher, metadata, setup helper and redistributable fallback audio libraries.
It does **not** contain SEGA's `libfox.so`, `data.obb`, APK files or runtime game
assets.

Required game version: **Android 3.0.0-109, arm64-v8a**. The older Android 2.0.0
APK and `main.22.com.sega.sonic4episode2.obb` belong to the armv7 engine and are
not compatible with this V6 package.

## First setup

Install the ZIP into the normal PortMaster directory, producing:

```text
roms/ports/Sonic4EP2.sh
roms/ports/sonic4ep2/
```

Put one complete, legally obtained 3.0.0-109 ARM64 source in
`roms/ports/sonic4ep2/`:

- `split_config.arm64_v8a.apk` and `split_packs.apk` from the same installation;
- one complete `.apks` or `.apkm` export containing those splits; or
- a merged APK containing both `lib/arm64-v8a/libfox.so` and
  `assets/data.obb`.

To obtain the official split paths from an Android device:

```text
adb shell pm path com.sega.sonic4episode2
adb pull "<path printed for split_config.arm64_v8a.apk>"
adb pull "<path printed for split_packs.apk>"
```

Both files must come from the same installed version. Keep a legal backup
outside the port directory because successfully consumed source archives may be
removed after setup to recover storage space.

Launch Sonic 4 EP2 from Ports. V6 opens the setup interface before the long
archive checks, reports validation and extraction separately, verifies the
installed library and data, and then starts the game. The visual interface is
best-effort feedback: if the active SDL/display backend cannot create it, setup
continues headless and writes its diagnostic log in the game directory.

Allow at least 1.5 GB of free space for first setup. The final user-supplied
library and data consume about 700 MB.

## Architecture

The native `sonic4.arm64` compatibility loader maps the user-supplied Android
`libfox.so`, provides the required JNI/Android services and presents through
SDL2 plus EGL/GLES. The port selects a real GLES context, adapts to the active
Wayland, KMSDRM or framebuffer environment, and uses the panel's native
resolution.

Audio is decoded through the port's native bridge. AArch64 fallback copies of
mpg123, libogg, libvorbis and libvorbisfile are included for minimal firmware
images; compatible firmware libraries remain preferred. Input and the Select +
Start exit combination use the native SDL gamepad path.

V6 incorporates the established fixes for texture-release lifetime, scene/FBO
clearing, title continuation, special-stage input, adaptive audio output and
Mesa/Panfrost GLES selection. The Android videos are not played by this port.

## Controls

| Control | Action |
|---|---|
| D-pad / Left stick | Move and navigate |
| A | Jump / confirm |
| B | Cancel |
| Start | Pause / title confirm |
| Select | Back |
| Select + Start | Exit to the frontend |

## Troubleshooting

- Missing ARM64 library: confirm that the source contains
  `split_config.arm64_v8a.apk`, not only the armeabi-v7a split.
- Missing game data: confirm that the same export contains `split_packs.apk`
  with `assets/data.obb`.
- Setup error: inspect `roms/ports/sonic4ep2/bake.log`.
- Launch, renderer, audio or controller error: inspect
  `roms/ports/sonic4ep2/log.txt`.
- A second launch must not require the APKs once both
  `lib/arm64-v8a/libfox.so` and `data/data.obb` are valid.
- Setup does not require GNU `stat`; on muOS it uses Python file sizes without
  rereading the complete OBB for every progress update.

## Build and package

The loader is built from `ports/sonic4` with the AArch64 release toolchain. The
release archive is generated only through the reproducible package builder:

```bash
cd ports/sonic4
./package/build-package.sh
```

The builder stages an explicit allowlist, verifies AArch64 ELF identity and a
maximum GLIBC requirement of 2.30, checks shell/Python syntax and metadata,
rejects proprietary data paths, creates a payload hash manifest, verifies ZIP
CRC and topology, and writes a sidecar SHA-256 file. `SOURCE_DATE_EPOCH` can be
set to reproduce a different fixed release timestamp.

## Source map

- `src/main.c`: loader lifecycle, game patches, input and frame loop.
- `src/setup_splash.c`: first-run setup renderer and progress protocol.
- `src/egl_shim.c`: SDL/EGL/GLES context and presentation handling.
- `src/jni_shim.c`: Android/JNI compatibility surface and local paths.
- `src/sonic_audio.c`: music and sound-effect decoding/output.
- `package/ports/Sonic4EP2.sh`: PortMaster launcher.
- `package/sonic4ep2/tools/sonic4ep2_extract.sh`: BYO-data installer.
- `package/build-package.sh`: deterministic, data-free release builder.

See `README-pt-BR.md` for Portuguese instructions and `licenses/` for source,
dependency, preview-asset and game-data notices.
