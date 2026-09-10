# Changelog

## 1.1.2

### Corrigido — menu sem texto no muOS (RG40XX-H)
- O zip 1.1.1 não embalava `chrono/fonts/` (o renderizador de manifesto só
  conhece o conjunto de arquivos do framework), e o muOS não traz nenhuma das
  fontes de firmware que o loader procura (Roboto/Noto/DejaVu/Liberation/
  FreeSans). Sem fonte, `createTextBitmap` devolvia bitmap vazio e a UI ficava
  sem texto. No ArkOS/dArkOS/NextOS a DejaVu do firmware mascarava o defeito.
- A Noto Sans (SIL OFL 1.1) agora vai **embutida no executável** (`.incbin`,
  `FT_New_Memory_Face`) como piso garantido — o texto não depende mais de
  arquivo ao lado do binário nem do firmware — e `fonts/` volta ao pacote.
  Ordem: `CHRONO_FONT` → `fonts/` do port → fonte embutida → fonte do firmware.
- O log passa a dizer qual fonte foi usada (`text_render: fonte ...`).

### Corrigido — não abria no ROCKNIX com Mesa/Panfrost (RG-DS)
- `NXLOADER prepare libchrono.so failed=unresolved strong import
  unresolved=eglGetProcAddress` (mais `glMapBufferOES`/`glUnmapBufferOES`):
  a `libGLESv2.so.2` do Mesa não exporta esses símbolos e a `libEGL` é aberta
  pela SDL com `RTLD_LOCAL`, então `dlsym(RTLD_DEFAULT)` não os enxerga. Nos
  blobs Mali tudo vive na `libMali` e por isso só o Mesa quebrava.
- Os símbolos GL/EGL do host agora são resolvidos por `dlsym` →
  `SDL_GL_GetProcAddress` (contexto já aberto) → `libEGL.so.1`; se ainda
  faltarem, `eglGetProcAddress` recebe um adaptador que consulta as mesmas
  fontes e `gl{Map,Unmap}BufferOES` recebem stubs (NULL/GL_FALSE), em vez de
  derrubar o carregamento inteiro.
- Bancada: `CHRONO_GLPROC_FORCE_SDL=1` ignora o `dlsym` e prova o caminho do
  Mesa num aparelho com blob Mali.

### Conhecido
- Com o APK PT-BR o nome padrão não aparece na tela de nome; confirmar
  "Aceitar" segue com "Crono". Só apresentação.

## 1.1.1

### Corrigido — o jogo não salvava com o APK traduzido (2.1.3 PT-BR)
- O launcher único (desde a 1.0.5) deixou de criar `chrono/userdata/`; o
  `run.sh` da 1.0.3 fazia `mkdir -p`. A build 2.1.3 (usada pelas traduções
  PT-BR) grava o save direto nesse diretório sem criá-lo, então o `fopen`
  falhava em silêncio e o menu nunca oferecia "Continuar". A retail 2.1.4/2.1.5
  cria o diretório sozinha, por isso só o APK traduzido sofria. O loader agora
  cria `userdata/` antes de entregar o caminho ao jogo.
- Medido no dArkOSRE (K36S) com o APK PT-BR 2.1.3: `Chrono_sp_6_0.dat`,
  `meta.bin` e `common.bin` gravados ao chegar ao quarto do Crono; segunda
  abertura mostra "Continuar" e carrega "A Feira Milenar / TEMPO 00:02".

### Corrigido — tela preta no filme de demonstração do título
- Builds com `Cocos2dxVideoHelper` (a 2.1.3 abre o `DemoMovieScene` após
  ~100 s parado no título) esperavam o evento COMPLETED do player Java, que o
  shim nunca enviava: a tela ficava preta até sair. O shim agora dá um índice
  a cada widget de vídeo e conclui todo `startVideo`/`resumeVideo` no quadro
  seguinte (o filme é pulado; nenhum decoder é fingido).

### Build
- `build_universal.sh` só aceita um toolchain NextOS cujo sysroot tenha os
  headers da SDL2 (um build 1.2.0 em andamento deixava um esqueleto).
- Bancada: `CHRONO_TAPFILE=<arquivo>` injeta um toque no centro (passa o
  "Toque para começar" sem tela sensível).

### Conhecido
- Com o APK PT-BR o nome padrão não aparece na tela de nome; confirmar
  "Aceitar" segue com "Crono". Só apresentação.

## 1.1.0

### Corrigido — saída SELECT+START (adota o chord canônico do framework)
- Remove a implementação própria de combo por evdev do `ct_platform.c` (que
  agregava todos os `/dev/input` globalmente e podia disparar com **START
  sozinho** no Knulli/família H700) e passa a usar o módulo canônico
  `nxinput_evdev_chord.h` v2 (nxinput 0.4.0):
  - **SDL é a autoridade** quando há pad aberto: o chord é
    `SDL_GameControllerGetButton(BACK) && (START)` por **estado** (+ botão cru do
    joystick nos índices do mapping). Dispara na hora, sem hold longo.
  - **evdev cru só como fallback** quando não há pad SDL; **nunca** vigia
    `BTN_SELECT/START` literais com pad aberto (na família H700 esses códigos são
    L2/R2).
- Sincroniza o `nxinput` vendorizado para 0.4.0 e adiciona `ct_framework_sdl_controller()`.
- Diagnóstico de controle completo no log (mapping, binds, códigos evdev).
- Provado no device (R36T/K36S, GO-Super, lançado pelo ES): **START sozinho NÃO
  sai**; **SELECT+START sai limpo (status 0, sem SIGSEGV)** com `evdev fallback
  muted`.

## 1.0.8

- Published the muOS installer correction under a new version instead of
  replacing or ambiguously reusing the existing 1.0.7 package number.
- Reproduced the failure against the official RG40XX-H muOS
  2601.1/Funky Jacaranda root filesystem: the older launcher exits before it
  can create its normal log because that image has no external `stat`
  command. The packaged launcher keeps the stat-free `ls` plus Bash `-ef`
  lock validation introduced in 1.0.7.
- Updated to canonical `nxbootstrap 0.6.6`. Any non-zero exit after Bash enters
  the launcher but before `log.txt` opens now leaves an owner-only,
  per-process `chrono-launcher-error.<pid>.log`, closing the silent-return
  diagnostic gap without claiming to cover interpreter or parse failures.
- Installed the release ZIP with the official PortMaster code from that muOS
  image, reopened PortMaster successfully with its generated version-4
  `port.json`, and launched the installed entry through to NXExtract. The game
  executable remains byte-for-byte unchanged.

## 1.0.7

- Regenerated the canonical `nxbootstrap 0.6.3` launcher so its instance lock
  no longer calls the external `stat` command, which is absent from some muOS
  images. It now reads the hardlink count through portable `ls` output and
  retains the Bash `-ef`, owner, regular-file, symlink and `flock` checks.
- Added release gates that reject any future generated launcher which brings
  the external `stat` dependency back.
- Preserved the approved `chrono-nextos` game executable byte-for-byte; this
  release changes only launcher/package metadata and documentation.

## 1.0.6

- Replaced the historical two-layer launcher with the canonical,
  self-contained `nxbootstrap 0.6.3` output. Its instance lock uses portable
  BusyBox `stat -t` plus Bash `-ef`, matching the fix physically confirmed on
  AmberELEC; no runtime `nxbootstrap.sh` is packaged.
- Renamed the public Linux executable to `chrono-nextos` and retained the
  public `GLIBC_2.30` ceiling.
- Added the visible `GAME_LANGUAGE="en"` selector. Supported values are the
  two modes proven in the Android binary, `en` and `ja`; the adapter converts
  `NXPORT_LANGUAGE` to the original location/region codes.
- Kept the original Android lifecycle, callbacks, graphics, audio and input
  flow unchanged.

## 1.0.5

Packaging-contract correction: the public ZIP now has one visible PortMaster
entry point, `Chrono Trigger.sh`. It contains the generated declarative config,
discovers `chrono/`, loads `chrono/nxbootstrap.sh` directly and calls
`nxbootstrap_main`. The redundant `chrono/run.sh` layer has been removed from
the source, package recipe and release gate.

- Runtime/game bytes are unchanged from the physically accepted build.
- The corrected ZIP passed deterministic host packaging, seven simulated ROM
  layouts, symlink and FAT-mode tests, and contains 21 entries with no
  `run.sh`.
- The exact corrected ZIP was then launched on the accepted NextOS Mali-450
  stack: PortMaster, NXExtract, JNI, GLES2/Mali video, PulseAudio and native
  input reached ready state; SIGTERM returned cleanly with no process left.
- Release v1.0.4 remains available as rollback evidence and was not
  overwritten.

Corrected ZIP SHA-256:
`2a9dec87e0742bf00b9e9f3af64e6b00218467e5f7665a769c687427a1f3277f`.

## 1.0.4

This is the publication release of the exact Chrono framework-pilot package
accepted on both supported stacks. The downloadable ZIP is byte-for-byte the
same package internally identified as 1.0.3; it was deliberately not rebuilt
after physical acceptance.

- The public repository now carries the runtime sources compiled into the
  port binary, vendored under `vendor/`: `nxloader`, `nxcompat`, `nxgl`,
  `nxinput` and `nxaudio`.
- The same ZIP was accepted on a NextOS Mali-450 stack with fbdev/PulseAudio
  and on an ArkOS RK3326 stack with KMSDRM/ALSA. Video, audio, native controls,
  the SELECT+START shutdown path and clean return were verified.
- The release asset is published with a SHA-256 companion file so the tested
  bytes can be checked after download.
- No proprietary game data is included. Installation still requires a
  user-owned Android APK and the bundled NXExtract flow.

Version 1.0.3 remains available as the rollback release. Tag 1.0.4 records the
source/framework promotion and publishes the already-tested package; it does
not claim a new game binary build.

## 1.0.3

The frame handed to the panel is now opaque. A player on an RG34XX-SP (muOS,
Mali-G31, SDL `mali` backend) reported that the screen never comes back after
the title fades out — with the engine plainly alive in the log: the scene
transition happens and the menu labels are built.

- **The backbuffer alpha is forced opaque immediately before the present.**
  Cocos leaves the game's own alpha in the default framebuffer. Measured on a
  Mali-450 at 1280×720: the menu frame carries **alpha 0 on 78.5 % of its
  pixels** and alpha 255 only where there is artwork; during the title fade it
  reaches 97.6 %. A compositor that ignores per-pixel alpha — the Amlogic OSD
  as NextOS configures it, and the opaque KMSDRM plane on ArkOS, the two
  devices this port was validated on — shows the picture anyway. A compositor
  that honours alpha reads the same frame as almost entirely transparent, and
  the panel goes black exactly when the title disappears.
- The clear reads the bound draw framebuffer, binds 0, writes **only** the
  alpha channel and puts back what it found, then logs once that it actually
  ran. A GLES3 driver can reach the swap with a non-zero FBO bound, which would
  send the clear to the FBO instead of the backbuffer and leave the flag on
  while nothing is fixed. The reporting device is GLES 3.2.
- `CHRONO_OPAQUE=0` turns it off for bench comparison.

Measured after the change: alpha is **255 on 100 % of the frame** and the RGB
content is byte-for-byte the same as before (21.4 % non-black on the menu, both
runs) — the clear touches nothing but alpha. Frame rate on the R36S over 150 s
per side: **60.0 fps steady with the fix, 42–45 fps without it**.

Also in this release: the Mali-450 regression pass is done, and the device
table now says so.

**Honest limit:** the port has no RG34XX-SP. This fix removes a measured defect
that explains the report on the hardware we do have; it has not been run on the
device that failed. Reports on Discord are welcome.

## 1.0.2

NXExtract recipe made tolerant, so a legitimate APK is never rejected for being
a different build of the same game.

- Assets are matched as `assets/*.dat` instead of a fixed `001`–`008` list: a
  build that renumbers, adds or drops a movie file still installs.
- Structural bounds widened everywhere (tree 8–96 files, 300 MB–1.6 GB;
  `libchrono.so` 6–48 MB; `resources.bin` 200 MB–1.2 GB). Nothing is pinned to
  one build's byte count.
- Required paths reduced to what every build has: `resources.bin`, `001.dat`
  and the shader tree. Region-dependent files (`007-en.dat`, `008.dat`) are
  installed when present and never demanded.
- The launcher's artefact gate matches that same list.
- Verified with `nxextract plan` against a real retail APK: the widened recipe
  resolves the identical 17 items / 564 MB as before.

Updating from 1.0.0/1.0.1 does not re-extract anything: the installer adopts
already-valid data and only rewrites the marker.

## 1.0.1

Portability audit against the multi-device contract; every fix below prevents a
"black screen" or "does not start" on hardware the release was never run on.

- **Video and audio now initialise as independent subsystems.** A single
  `SDL_Init(VIDEO|AUDIO|...)` meant a dead inherited PulseAudio took the whole
  boot down and the port never drew a frame. Audio failure is now scoped: the
  game starts, with picture, and says in the log that it has no sound.
- **Audio ladder**: `dummy` and `disk` no longer count as success, and one
  logged retry drops an invalid inherited `SDL_AUDIODRIVER`/`PULSE_SERVER`
  before giving up.
- **EGLConfig fallback ladder** (alpha 8→0, depth/stencil 24/8→16/0→0/0). The
  first rung is exactly the configuration validated on Mali-450 and on the R36S;
  a driver that refuses it now gets the next rung instead of a fatal error.
- **Desktop GL is rejected.** If the driver hands back a non-ES context, the
  context is recreated once against the GLES driver; Cocos2d-x shaders are GLSL
  ES and would otherwise render black.
- **Inherited `SDL_VIDEODRIVER`** that fails a real probe is dropped once, giving
  autodetection back to SDL — the backend is never chosen by us.

## 1.0.0

First universal BYO-data release of the Chrono Trigger compatibility loader.

- Single AArch64 executable built against GLIBC 2.27 (public ceiling: 2.30);
  SDL2, GLESv2 and FreeType come from the target firmware.
- Runtime capability detection: ROM root, real drawable size, SDL video/audio
  backend chosen by the firmware, PortMaster control mapping. No device-name
  profiles, no hardcoded resolution, no forced SDL backend.
- `glFinish` before the buffer swap only on KMSDRM (30 → 60 fps there, no cost
  on fbdev).
- Exact EGLConfig logged once (RGBA8888 vs. the driver's RGBX8888 default).
- Single instance enforced with `flock` on the executable itself.
- SELECT+START (SDL and raw evdev, including TRIGGER_HAPPY1/2), SIGTERM and the
  engine's own quit paths all converge on one shutdown: pause/save, then exit,
  with a deadline so nothing keeps the display or audio device.
- UI font resolved at runtime: bundled Noto Sans (SIL OFL 1.1) or a suitable
  firmware font. No Android system font is redistributed.
- BYO-data installation through NXExtract 1.2.3, with a structural recipe that
  accepts every known Play build instead of a single hash.
