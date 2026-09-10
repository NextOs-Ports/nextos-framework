# Changelog

## 1.0.3 — 2026-08-08

- Switched the release ZIP to the canonical PortMaster layout used by every
  other published NextOS port: `Prizefighters 2.sh` and `pf2/` at the root,
  unzipped straight into `roms/ports/`. v1.0.2 wrapped the release in
  `ports/` + `ports_scripts/`, so on muOS the port had no visible entry at
  all, and moving only the `.sh` by hand left the game folder unreachable.
- Taught the entry script to find `pf2/` on muOS/ArkOS/ROCKNIX/Batocera card
  roots (`mmc`, `sdcard`, `union`, `/roms`, `/roms2`, `/userdata`) as well as
  beside the script and one level up.
- Removed the silent failure: when the game folder or `bash` is missing, the
  launcher writes `pf2-launcher-error.log` next to the script and prints the
  reason on the console instead of returning instantly to the ports list.
  Errors raised before the `pf2.log` redirect are reported the same way.
- Documented the per-firmware install layout, including the muOS
  `mmc/roms/ports` path.

## 1.0.2 — 2026-08-02

- Normalized face buttons by physical position on legacy HID-order kernels
  with the proven NextOS ordinal-pad detector used by the approved GTA ports.
- Preserved existing SDL/PortMaster mappings on devices with modern semantic
  button codes and kept `PF2_PAD_MAP` as the explicit override.
- Documented the R36S/Nintendo labels as Y/X/B/A for left high, right high,
  left middle/body and right middle/body punches respectively.
- Replaced frame-dependent cursor movement with radial deadzone, progressive
  response, smoothing and elapsed-time integration.
- Raised the default pointer speed and exposed it as `PF2_CURSOR_SPEED=1400`
  in the launcher, adjustable without rebuilding.

## 1.0.1 — 2026-08-02

- Synchronized the vendored extractor with canonical NXExtract 1.2.0.
- Added the canonical firmware-first `nxextract-runtime-env.sh` boundary.
- Prevented game-private `LD_LIBRARY_PATH` entries and their symlink targets
  from leaking into the extractor/UI process.
- Preserved valid inherited SDL video/audio choices without forcing a backend.
- Added precise diagnostics when an APK/XAPK member matches a source pattern
  but fails size, SHA-256, CRC32 or ELF validation.
- Added the upstream runtime-boundary regression test to the release gate.
- Passed the canonical `suportando_outros_devices` portability auditor.

## 1.0.0 — 2026-08-02

- First private universal ARM64 release.
- Preserved Mali-450/fbdev GLES2 while adding SDL-owned KMSDRM/Wayland contexts.
- Added device-independent pointer coordinates, uGUI hit testing and A/R3 click.
- Added PortMaster-normalised controls, raw-pad fallback and Select+Start exit.
- Added controller-operated pixel keyboard with focused InputField commit.
- Added native FMOD-to-SDL audio and persistent typed SharedPreferences.
- Added NXExtract 1.1.2 XAPK discovery, exact v1.09.3 validation, resumable
  staging and atomic commit.
- Replaced unreliable on-device PairIP execution with exact-input,
  SHA-validated transformation masks.
- Bundled Python-3.7-compatible UnityPy 1.22.5 and LZ4 for GLES2 shader setup.
- Added backed-up owner-save import that excludes device-resolution keys.
- Added a single GLIBC <=2.30 package for external handheld firmware.
