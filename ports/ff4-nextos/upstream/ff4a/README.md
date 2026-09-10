# Final Fantasy IV: The After Years — Android v1.0.12 (aarch64 so-loader, GLES1)

A native Linux host for the Android build of *Final Fantasy IV: The After
Years*, built for handheld CFW devices. The port carries **no game data**: the
engine, the OBB, the art and the audio belong to Square Enix and must be
supplied from an APK you legally own.

Sibling port: `ports/ff4` (*Final Fantasy IV 3D Remake*), which shares the same
host design.

## English

### Architecture

The Android runtime is replaced, not emulated. The port is a normal aarch64
executable that:

1. opens a window and a GLES1 context through SDL2;
2. maps `libff4a.so` with its own ELF64 loader (`src/so_util.c`), a single
   module, layout-agnostic relocation including compact `RELR`;
3. answers the engine's JNI upcalls with a false `JNIEnv`/`JavaVM`
   (`src/jni_shim.c`) — viewport, key events, file/sound/texture loading, font
   drawing, save-file naming and the frame limiter;
4. decodes the encrypted OBB natively (`src/obb_data.c`): LCG-XOR `encode`,
   key `offset + 0x5E51D48`, binary index of 12 bytes per entry, then gunzip;
5. implements OpenSL ES over SDL audio (`src/opensles_shim.c`), 32 kHz stereo.

### Graphics

The engine is OpenGL ES 1.1 fixed-function. The port does **not** link
`libGLESv1_CM`, because that SONAME cannot be trusted. On an R36S running
dArkOS the names are crossed: `libGLESv1_CM.so.1` is a 198 KB driverless Mesa,
while the real 40 MB Mali blob sits behind the unversioned `libGLESv1_CM.so`
and behind `libmali.so`. Binding the versioned name yields a context that
accepts every call and draws nothing. GLES1 is therefore resolved at run time
by `framework/nxgl` (`nxgl_gles1`), from the source most coherent with the live
context first, and the port logs which provider answered.

### Fixed problems

| Symptom | Cause and fix |
|---|---|
| game and music running at 2× | `getCurrentFrame(prev)` is the engine's frame limiter and blocks until the wallclock tick passes; the shim was returning a free-running 60 Hz counter. Now it blocks on `CLOCK_MONOTONIC` and honours `setFPS`. |
| audio stuttering and fast | a race in the lazy open of the SDL device let the engine thread and the pump thread both pass `if (!g_dev)`, so two devices drained one ring. The open is now under a mutex. |
| `B` did not go back in menus | on Android the back action came from the physical BACK button (bits `0x8000`/`0x4000` depending on `assignBackButton`). The shim now tracks the mode and sends `K_B` plus the matching bit. |
| direction repeating forever | `render()` does `cont \|= getKeyEvent()` every frame and nothing cleared `cont` on the pad path (on Android `touch()` cleared it). The port now clears the released bits each frame. |
| would not load on Mali G31 | see **Graphics** above. |

### Controls

D-pad and left stick move; `A` confirms, `B` cancels and goes back, `X`/`Y`
and the shoulders map to the engine's own buttons. Some mobile menus are
touch-only in this build and are driven by a cursor on the right stick, with
`A` acting as a tap.

**`SELECT` + `START` closes the game.**

### Required game data

From the `arm64-v8a` APK of `com.square_enix.android_googleplay.FF4AY_GP`:

```
ff4a/
├── libff4a.so            # lib/arm64-v8a/libff4a.so
└── data/
    └── main.obb          # assets/main.obb
```

NXExtract does this for you on the first launch — drop the APK into
`gamedata/` and start the port. The Japanese package
(`com.square_enix.FF4AY_J`) is not accepted. See `INSTALLATION.md`.

### Build

```
./build_universal.sh      # aarch64, GLIBC <= 2.30, in the pinned Buster image
```

The build fails closed if the binary exceeds `GLIBC_2.30` or if it acquires a
`DT_NEEDED` outside the universal baseline.

### Source map

- `src/main.c` — window, GLES1 context, input, cursor, exit chord, main loop
- `src/jni_shim.c` — false JNI environment and the engine's upcalls
- `src/so_util.c` — ELF64 loader and symbol resolution
- `src/imports.c` — bionic-only symbols the glibc does not provide
- `src/opensles_shim.c` — OpenSL ES over SDL audio
- `src/obb_data.c` — OBB decode pipeline
- `src/texture.c`, `src/crash.c`, `src/util.c`, `src/error.c` — support

### Licenses

The host runtime is GPL-3.0-only (`LICENSE`). `stb_image.h` and
`stb_truetype.h` are public domain. NXExtract is MIT
(`licenses/NXExtract-MIT.txt`). Final Fantasy IV: The After Years, the engine,
the OBB, art and audio belong to Square Enix and are neither licensed nor
distributed by this repository.

## Português

Host nativo Linux para o build Android de *Final Fantasy IV: The After Years*,
feito para portáteis com CFW. O port **não** traz dado de jogo: engine, OBB,
arte e áudio pertencem à Square Enix e vêm de um APK adquirido legalmente.

### Arquitetura

O runtime Android é substituído, não emulado: janela e contexto GLES1 pela
SDL2, `libff4a.so` mapeada por um loader ELF64 próprio, `JNIEnv` falso
respondendo às upcalls da engine, OBB decodificado nativamente e OpenSL ES
sobre o áudio da SDL.

### Gráficos

A engine é GLES1 fixed-function. O port **não** linka `libGLESv1_CM` porque
esse SONAME não é confiável: no R36S com dArkOS os nomes estão cruzados — o
`libGLESv1_CM.so.1` é uma Mesa de 198 KB sem driver, e o blob Mali real de
40 MB está atrás do nome sem versão e do `libmali.so`. Quem resolve em tempo de
execução é o `framework/nxgl` (`nxgl_gles1`), da fonte mais coerente com o
contexto vivo para a menos coerente.

### Controles

D-pad e analógico esquerdo andam; `A` confirma, `B` cancela e volta. Alguns
menus mobile são de toque e são operados por cursor no analógico direito, com
`A` valendo como toque.

**`SELECT` + `START` fecha o jogo.**

### Dados necessários

Do APK `arm64-v8a` de `com.square_enix.android_googleplay.FF4AY_GP`:
`libff4a.so` na raiz do port e `data/main.obb`. O NXExtract faz isso na
primeira abertura — basta largar o APK em `gamedata/`. A variante japonesa não
é aceita.

### Compilar

```
./build_universal.sh
```

Falha fechada se o binário passar de `GLIBC_2.30` ou ganhar um `DT_NEEDED`
fora da linha de base universal.
