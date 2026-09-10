# Device compatibility

This is a living compatibility matrix. “Verified” means the listed release
completed install, full verify and marker-based second run with a synthetic
renamed package. A release column prevents an older result from being
mistaken for a test of newer code.

“Maintenance revalidated” means the preceding verified release remains the
full synthetic baseline and the maintenance release completed its changed
path, full output verification and marker path on the listed real device.

No private IP address, hostname, credential or raw log belongs in this file.

| Family / OS | Arch | glibc | Python | SDL video | UI backend | Visual check | Release / status |
|---|---|---:|---:|---|---|---|---|
| NextOS Amlogic-old / Mali-450 | AArch64 | 2.43 | 3.14.6 | `mali`, `offscreen` | `mali` | 1280×720 capture inspected | 1.1.1 maintenance revalidated |
| NextOS X5M / Mali-G310 Valhall | AArch64 | 2.43 | 3.14 | KMSDRM/Wayland-class | `KMSDRM` | 1920×1080 backend opened | 1.1.1 maintenance revalidated |
| R36S / ArkOS | AArch64 | 2.30 | 3.7.5 | `KMSDRM`, `offscreen` | `KMSDRM` | 640×480 capture inspected | 1.1.1 maintenance revalidated |
| K36S/R36S-class / dArkOSRE | AArch64 | 2.41 | 3.13.5 | KMSDRM advertised, EGL/GL unavailable | active `tty1` | 80×30 console inspected | 1.2.7 historical result; rejected as public visual baseline |

The release UI has deterministic, architecture-matched AArch64, ARMv7, x86_64
and i386 artifacts with an automatic GLIBC 2.30 ceiling. The current artifacts
require at most GLIBC 2.17.

NXExtract 1.2.0 changes the Python planner and shell integration, not the UI
binary. Its release gate covers the firmware-first child scope, removal of
game-private library paths, preserved Wayland/KMSDRM inheritance and untouched
SDL autodetection. The table retains the last physical device release instead
of presenting host regression tests as new device validation.

NXExtract 1.2.6 still uses the byte-identical GLIBC 2.17 UI asset published in
1.1.0 (SHA-256
`046afb583f5a211c946495e639409f81d9cfec706788eeccb7924b0e8e5a50b6`).
Its 56-test transaction/security suite and ELF/runtime gates are host evidence,
not a fresh physical 1.2.6 result. The table therefore deliberately remains on
the last release actually exercised on each device family.

NXExtract 1.2.7 changes the UI bytes and packaged-runner contract. Its AArch64
ELF remains GLIBC 2.17 and has SHA-256
`b4daf1bdffe4f1623752742bc6b796f93a203f8e4cba4c39f62dfcfd37cc2d72`.
The dArkOSRE row records the regression that led to 1.2.8; it is negative
historical evidence, not an accepted public interface. Exact game ZIPs still
need their own first-install and runtime acceptance.

NXExtract 1.2.9 addresses a later ArkOS full-launcher observation: all 30
bounded SDL window attempts reported an unavailable EGL/GL provider and the
approved graphical fbdev renderer arrived only near the former 20-second
deadline. The 40-second fail-closed engine boundary covers that path. The UI
also reuses the portable EGL/GLES-name recovery already physically validated by
NXSplash 0.1.2 on that family. The exact 1.2.9 UI artifact now has the isolated
component receipts recorded below. Only the Mali-450 receipt includes a valid
graphical capture; KMSDRM scanout cannot be inferred from stale framebuffer
bytes. These receipts do not claim installation, marker reuse, game runtime or
a universal port; each game ZIP still requires its own physical acceptance.

## Verified records

### R36S / ArkOS — release 1.1.0

- The complete Python suite passed under the device's Python 3.7.5.
- A randomly named XAPK containing an arbitrarily named inner APK was
  discovered by content and installed for `arm64-v8a`.
- The synthetic bake, full verification and marker fast path passed.
- The input SHA-256 remained unchanged and no stage, source cache, journal,
  backup or extractor process remained after completion.
- SDL advertised `KMSDRM` and `offscreen`; NXExtract selected visible
  `KMSDRM`. A real [640×480 scanout capture](images/nxextract-kmsdrm-640x480.png)
  confirmed the responsive header, phase bars, detail and overall progress.
- During a laboratory visual test, the frontend released DRM master first.
  This is not a launcher requirement and service control is never distributed.

### NextOS Amlogic-old / Mali-450 — release 1.1.0

- Install, full verification, source preservation and marker fast path passed.
- The firmware SDL advertised `mali` and `offscreen`; NXExtract selected the
  visible `mali` backend.
- A real [1280×720 framebuffer capture](images/nxextract-mali-1280x720.png)
  confirmed the responsive header, phase bars, detail and overall progress.

### NextOS X5M / Mali-G310 Valhall — release 1.0.0

- Install, full verification, source preservation and marker fast path passed.
- NXExtract selected visible `KMSDRM`.

### Release 1.1.1 maintenance revalidation

- The new per-ABI adoption-rejection diagnostic passed its synthetic regression
  test in the complete 22-test release suite.
- On R36S/ArkOS and NextOS Mali-450, 1.1.1 strictly adopted validated existing
  data, wrote a new marker, completed a full verify and accepted the subsequent
  marker path.
- On NextOS Mali-G310, 1.1.1 accepted the existing marker and completed a full
  verify.
- All three foreground runs exited cleanly, left no extractor or game process
  and restored the frontend state used before testing.
- The SDL UI binary is byte-identical to 1.1.0; no rendering code changed in
  this maintenance release.

### Quarantined dArkOSRE active-TTY fallback — release 1.2.7

- The same SDL/KMSDRM path used by 1.2.6 was attempted first and failed real
  window creation because EGL/GL could not be loaded.
- The new helper immediately selected the kernel-published active `tty1`, then
  wrote its private `visible=tty` readiness proof.
- The 80×30 virtual-console buffer was inspected while the process remained
  live. It contained the game and recipe identities, extraction phase, current
  file, phase/overall percentages, bilingual wait text and RETRO ELITE footer.
- The helper stopped through its normal stop file and left no UI process.
- This record contains no device address and does not promote the direct UI
  probe into full game-release acceptance.
- Release 1.2.8 never accepts this path as public readiness. TTY output is
  diagnostic-only behind `NXEXTRACT_ALLOW_TTY_UI=1`; the packaged runner fails
  closed unless the approved graphical layout is actually presented.

### NXExtract 1.2.9 isolated renderer receipts

- On NextOS Amlogic-old / Mali-450, the exact 1.2.9 AArch64 UI reported the
  private `visible=sdl` readiness proof through SDL's `mali` driver. A physical
  capture was inspected and confirmed the approved graphical NXExtract layout.
- On the authorized ArkOS-class AArch64 / KMSDRM target, the exact 1.2.9 UI
  reported `visible=sdl` through SDL KMSDRM after the capability-based portable
  EGL/GLES provider recovery. The log identified the KMSDRM renderer. No valid
  visual capture exists: `/dev/fb0` retained an older frame because DRM owned
  scanout, so framebuffer bytes are explicitly not accepted as visual proof.
- Both probes were stopped cleanly and left no NXExtract/UI process behind.
- These are component-only renderer receipts. They deliberately do not promote
  a game ZIP, installation flow or full-game run to verified status, and the
  ArkOS-class result does not claim that the layout was visually inspected.

## Validation contract

For each new family:

1. record architecture, glibc, Python and advertised SDL drivers;
2. use a synthetic APK/bundle whose external and inner filenames are arbitrary;
3. run first install with the UI visible;
4. run `verify`;
5. run install again and confirm marker fast-path;
6. confirm the original source hash did not change;
7. confirm no transaction, UI or hook process remains;
8. restore the frontend state used before the test.

## Known display behavior

- Mali/fbdev firmware generally auto-selects `mali`.
- Direct DRM firmware generally auto-selects `KMSDRM` once the frontend has
  released DRM master.
- If SDL advertises a plausible backend but real window creation still fails,
  1.2.9 first tries portable EGL/GLES provider names when no explicit choice
  exists, retains the approved bounded retries, and then attempts the same
  graphical layout through the active framebuffer.
- Wayland is attempted only when a real Wayland socket exists.
- An `offscreen` or `dummy` driver is never accepted as visible success.
- Direct developer calls may remain headless when `--require-ui` is absent.
  The packaged runner always requires readiness and fails before extraction if
  neither SDL nor the graphical framebuffer renderer opens.

Service control is a laboratory-only test step. It must never be copied into a
distributed port launcher.

## Report template

```text
Family / OS:
Architecture:
glibc:
Python:
SDL drivers:
Selected backend:
Resolution visibly checked:
Install:
Full verify:
Second run:
Source hash preserved:
Required workaround:
```
