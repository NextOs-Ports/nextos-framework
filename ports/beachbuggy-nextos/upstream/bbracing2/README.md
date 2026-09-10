# Beach Buggy Racing 2 — NextOS universal port

## English

### Overview

BYO-data port of **Beach Buggy Racing 2** (Vector Unit) for aarch64 CFW
handhelds. It runs the original Android ARM64 `Vu*` engine through a Linux
so-loader with the NextOS port framework: nxbootstrap launcher, NXExtract
BYO-data installation, nxgl runtime GLES2 resolution and continuous image
proof. This is not a Unity game; the engine is Vector Unit's own C++ engine
on SDL3 + FMOD Studio.

No game data is included. You supply the APK of a copy you legally own; the
extractor identifies, validates and installs it on the first launch. See
`INSTALLATION.md`.

### Architecture

```text
nxbootstrap launcher (single visible .sh)
  -> NXExtract (first launch only: APK -> gamedata/)
  -> nxsplash (mandatory 5 s splash)
  -> bbracing2 loader (aarch64, GLIBC <= 2.30)
     -> bundled SDL3 (KMSDRM + ALSA/Pulse)
     -> nxgl GLES2 runtime resolution (no GL DT_NEEDED)
     -> EGL bridge measured per firmware (dArkOS/ROCKNIX mirrors)
        -> original ARM64 Vu engine (SDL_main)
```

### Solved problems

- GLES2 and EGL resolved at run time by measurement — no GL/EGL `DT_NEEDED`,
  so the same binary loads where the SONAMEs are crossed (dArkOS x ROCKNIX).
- The engine's pre-window EGL capability probe (display + configs + pbuffer
  context) satisfied on GBM/surfaceless platforms; `EGL_NV_system_time`
  emulated over `CLOCK_MONOTONIC`.
- SDL3 bundled (no CFW ships SDL3), with the nxsplash portable-provider
  retry for firmwares whose versioned EGL SONAME is a dead GLVND dispatcher.
- Android bionic stack-guard TLS slot stabilized under glibc (the classic
  false "stack smashing detected").
- Android/glibc pthread compatibility, FMOD PCM audio bridge over SDL3,
  AAsset bridge for `Assets.apf`.
- Safe `SELECT+START` exit in the real SDL event path, with frame proof
  published on every exit path.

### Controls

- Left stick / D-pad: steering
- `A`: confirm / use power-up
- `R1`: accelerate · `L1`: brake · right stick: menu cursor · `R2`/`R3`: cursor click
- `START`: pause
- `SELECT+START`: exit

### Build

`./build_universal.sh` builds the public loader (Debian Buster cross
toolchain, GLIBC <= 2.30, gates on DT_NEEDED closure). `./build.sh` builds
the NextOS Elite variant against the NextOS sysroot. The framework tree is
required (`framework/` at the repository root).

## Português

### Visão geral

Port BYO-data de **Beach Buggy Racing 2** (Vector Unit) para portáteis aarch64
com CFW. Roda a engine Android ARM64 `Vu*` original num so-loader Linux com o
framework de ports NextOS: launcher nxbootstrap, instalação BYO-data pelo
NXExtract, resolução GLES2 em runtime pelo nxgl e prova de imagem contínua.
Não é Unity; é a engine C++ própria da Vector Unit sobre SDL3 + FMOD Studio.

Nenhum dado do jogo vem incluído. Você fornece o APK de uma cópia adquirida
legalmente; o extrator identifica, valida e instala na primeira abertura.
Ver `INSTALLATION.md`.

### Problemas resolvidos

- GLES2 e EGL resolvidos em runtime por medição — sem `DT_NEEDED` de GL/EGL,
  o mesmo binário carrega onde os SONAMEs estão cruzados (dArkOS x ROCKNIX).
- Probe de capacidade EGL da engine (display + configs + contexto pbuffer)
  satisfeito em GBM/surfaceless; `EGL_NV_system_time` emulado.
- SDL3 embutida (nenhuma CFW traz SDL3), com o retry de provedor portátil do
  nxsplash para firmwares com EGL versionada morta.
- Slot TLS da stack-guard bionic estabilizado sob glibc (o clássico
  "stack smashing detected" falso).
- Compatibilidade pthread Android/glibc, ponte de áudio FMOD sobre SDL3,
  ponte AAsset para o `Assets.apf`.
- Saída segura por `SELECT+START` no caminho real de eventos SDL.

### Controles

- Analógico esquerdo / direcional: direção
- `A`: confirmar / power-up · `R1`: acelerar · `L1`: frear · analógico direito: cursor · `R2`/`R3`: clique
- `START`: pausa · `SELECT+START`: sair

### Build

`./build_universal.sh` gera o loader público (toolchain cruzado Debian
Buster, GLIBC <= 2.30, com travas de DT_NEEDED). `./build.sh` gera a
variante NextOS Elite contra o sysroot do NextOS. A árvore do framework é
necessária (`framework/` na raiz do repositório).
