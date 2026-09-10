# Beach Buggy Racing + Beach Buggy Racing 2 — NextOS universal ports

Native AArch64 so-loader ports of **Beach Buggy Racing** and **Beach Buggy
Racing 2** (Vector Unit) for handheld Linux CFW — NextOS, dArkOS/R36S and
other aarch64 firmwares — built on the NextOS port framework: nxbootstrap
launcher, NXExtract BYO-data installation, runtime GLES2/EGL resolution by
measurement, bundled SDL3 and continuous image proof.

**No game data is included or distributed.** Each port installs itself from
the APK of a copy you legally own, validated by content hashes on the first
launch. See each port's `INSTALLATION.md`.

| Port | Directory | Engine |
|---|---|---|
| Beach Buggy Racing | [`beachbuggy/`](beachbuggy/) | Vector Unit `Vu*` (C++, SDL3 + FMOD) |
| Beach Buggy Racing 2 | [`bbracing2/`](bbracing2/) | Vector Unit `Vu*` (C++, SDL3 + FMOD) |

The GPL-3.0 adapter source lives in each port's `src/`. `build_universal.sh`
builds the public loader with a Debian Buster cross toolchain (GLIBC <= 2.30);
`build.sh` builds the NextOS Elite variant. Both builds resolve OpenGL ES 2.0
and EGL at run time — no GL `DT_NEEDED` — so the same binary loads on
firmwares whose GL SONAMEs are crossed in opposite directions.

Beach Buggy Racing and Beach Buggy Racing 2, their engines, art and audio
belong to Vector Unit. This repository contains only the port runtime.

---

Ports nativos AArch64 de **Beach Buggy Racing** e **Beach Buggy Racing 2**
(Vector Unit) para portáteis Linux com CFW, sobre o framework de ports
NextOS. **Nenhum dado do jogo vem incluído**: cada port se instala a partir
do APK de uma cópia adquirida legalmente, validado por hash na primeira
abertura (`INSTALLATION.md` de cada port).
