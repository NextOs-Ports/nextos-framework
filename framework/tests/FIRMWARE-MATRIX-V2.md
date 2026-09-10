# Firmware test matrix v2

This matrix extends the original firmware-contract fixture; it does not replace
it. The v1 profile and launcher tests remain unchanged so previously approved
behavior stays visible.

The v2 profile set separates muOS, ROCKNIX/Panfrost, AmberELEC,
Knulli/Batocera, ArkOS, dArkOSRE, NextOS/Mali-450, TrimUI and spruceOS/Miyoo Flip. The evidence level
on each row comes only from the pinned PortMaster sources, the sanitized device
catalog and the approved release ledger. A design-only row remains design-only.

## What the automatic gates prove

`test_firmware_matrix_v2.py` is process-free. It verifies:

- profile roots and `control.txt` paths against the pinned PortMaster contract;
- safe CFW names and the two-regular-marker boundary for dArkOSRE;
- paired AArch64/ARMv7 routing, `PORT_32BIT`, PortMaster library suffixes and
  per-role NXExtract/NXSplash ELF classes and machines, including the Spruce
  AArch64-extractor/ARMHF-game split;
- the existing focus-loss, disconnect and hotplug contract from the approved
  M15 input evidence;
- a receipt schema in which `synthetic` requires `hardware_ran=false` and
  `device_access=false`, while `physical` requires both to be true plus an
  explicit authorization reference and exact artifact SHA-256.

`run-firmware-matrix-v2.sh` is called only from the already sealed
nxbootstrap user/PID/mount namespace. For every architecture row it creates a
second disposable bubblewrap root with networking disabled and mounts the
profile's real logical PortMaster and ports paths. It then runs the generated
launcher twice:

1. normal child exit, checking status 37, mapping, stick hint, ABI library
   precedence and one `pm_finish`;
2. injected pre-runtime failure, checking status 1, an owner-only `0600`
   launcher error log with all required fields, and one `pm_finish`.

The synthetic `PATH` contains only the commands the launcher needs and has no
`stat` entry. UI binaries are not executed in this headless fixture: their
architecture, size and SHA-256 are checked against the immutable component
manifests, while their renderer/pixels remain covered by the dedicated visual
gates and physical acceptance.

## Device-faithful runtime fixtures and field cases (E5)

Profiles may declare `runtime_fixtures` (real files extracted from official
firmware images, mounted at their real device paths) and `field_cases`
(named field-bug classes proven test-first inside the sandbox):

| Profile | Fixture | Field case | Bug class it seals |
|---|---|---|---|
| `amberelec` | byte-identical `/etc/openal/alsoft.conf` (`drivers=alsa`) from AmberELEC-RG351P 20230203 | `embedded-openal-shield` | embedded OpenAL guest goes MUTE reading the host config (Tightrope); the launcher shield must pin `ALSOFT_DRIVERS=opensl` and print the ENV RECEIPT; the standard no-capability run must stay untouched |
| `muos` | `python3` do firmware stubado por caso ("bad marshal data" na stdlib) | `python-probe` | NXExtract morria com erro críptico quando o python3 do firmware não iniciava; o launcher agora prova o interpretador ANTES: cache privada resolve (WARN e segue) ou mensagem clara 6209 |
| `spruce` | external spruce 4.3.4 contract: default interpreter absent, alternate loader at `/mnt/SDCARD/spruce/flip`, persistent ARMHF chroot and separate muOS `usr/lib32`/`usr/lib` worlds | `armhf-interp-preflight`, `spruce-mixed-abi-runtime` | an ARMHF role must use the off-path loader and ARMHF-only closure while NXExtract remains AArch64; the target SDL reports `KMSDRM,dummy`, with KMSDRM physically selected |

Golden rule: every new field bug earns one fixture that FAILS without the
class fix and PASSES with it, plus its inverted mirror.

The normal matrix keeps only a synthetic loader-routing stub. The optional
[`device-environments/spruce`](device-environments/spruce/README.md) preparer
consumes the exact external firmware archive, verifies every pinned source,
builds the mixed-ABI tree outside Git and runs a small ARMHF SDL query through
the real loader with QEMU. It never initializes graphics or turns a PC result
into device evidence.

## Device-control matrix (E7)

`test-device-controls.sh` + `fixtures/controls/controls-v1.json`: each case
carries a device's REAL evdev key table (captured live via EVIOCGBIT or from
the firmware image's DTS) plus the SDL mapping its frontend actually exports,
and declares which authority MUST own SELECT+START (SDL state vs the raw evdev
fallback) and through which codes. Virtual SDL joysticks replay the mapping;
no hardware runs. Both field regressions are sealed test-first: the bindless
frontend mapping (chord died muted) and the H700 gpio table where the literal
BTN_SELECT/START codes are physically L2/R2. **New device supported = capture
its key table + frontend mapping into a new fixture case FIRST.**

## Mandatory physical release checklist (device)

A port release is INVALID until, on real hardware and **launched from the
frontend** (its exported controller mapping is part of the test):

0. **from-scratch install (rule #41, 2026-08-19 compat crisis)**: delete the
   whole previous install first (inner port folder AND the root launcher
   `.sh`), extract the candidate ZIP exactly like a player would, place the
   owner APK, and watch the FULL extraction reach `NXE0000` with the
   `NXEXTRACT_RESULT` line in the log. A run that reuses a previous
   fast-marker (`NXE0001`) does NOT count as an install test. A second launch
   must then take the fast path. Field lesson: hybrid installs (old launcher +
   new engine) only ever appeared in the field because bench tests validated
   on top of stale installs;
1. installs (auto-install), boots to a drawn frame, plays audio;
2. the controller is detected (chord diagnostic lines present in the log:
   `EXIT chord SDL: ... back=... start=...`);
3. START alone does NOT close the game;
4. **SELECT+START exits cleanly (status 0) — proven by hand or by a uinput
   virtual pad on the device**. A boot-test without the chord proof is not a
   validation (field lesson: a release shipped with the exit chord dead while
   boot, audio and video were all green).

ROCKNIX/Panfrost is explicitly `physical-runtime-ui-unverified`: approved game
runtime evidence cannot be reused as graphical NXExtract evidence. Its
historical first extraction completed with a black NXExtract UI, so only a new
authorized physical renderer/pixel acceptance can change that UI status.

## Receipts and evidence boundary

The process gate produces
`nxframework-firmware-matrix-receipt-v2`. By default it validates the receipt
inside its private temporary tree and the durable gate log records the result.
For a local diagnostic artifact outside the canonical full runner, an absolute,
non-existing destination can be supplied:

```sh
bash framework/tests/run-firmware-matrix-v2.sh \
  --receipt /absolute/new/firmware-matrix-receipt.json
```

That command still refuses to run outside the sealed test namespace. It is not a
device harness. The automatic receipt always says `evidence.kind=synthetic`,
`hardware_ran=false`, `device_access=false` and `universal_evidence=false`.
No IP, remote command, firmware image or device credential exists in these
fixtures.

Physical validation remains a separate, explicitly authorized session using the
same final ZIP/SHA. A physical receipt can never be synthesized by this runner;
its distinct schema branch requires authorization and artifact identity, and
still does not turn one device result into universal evidence.
