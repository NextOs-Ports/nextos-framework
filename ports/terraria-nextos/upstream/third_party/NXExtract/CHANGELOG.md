# Changelog

All notable NXExtract changes are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 1.2.2

- Guarantee the fullscreen setup UI can never outlive a failed run and leave
  the device unable to shut down cleanly. The UI now installs SIGTERM/SIGINT/
  SIGHUP handlers that exit through the normal SDL teardown (restoring the VT,
  DRM master and evdev grabs), asks the kernel for SIGTERM on parent death
  (`PR_SET_PDEATHSIG`), notices an orphaned state via `getppid()`, and honors
  a wall-clock deadline (`NXEXTRACT_UI_MAX_SECONDS`, default 3600 s).
- Keep the parent's UI kill ladder running even when the polite stop file
  cannot be written — a full SD card is exactly the state a failed install is
  most likely in. The final `SIGKILL` wait is also bounded now.
- Convert SIGTERM/SIGINT/SIGHUP delivered to the extractor into a normal
  error return so the UI teardown and the workspace lock release always run,
  including when a launcher or CFW shutdown script times the extractor out.

## 1.2.1

- Never fail an install because the scratch source cache could not be deleted.
  The cache is now dropped after the payload is committed and after every
  source archive is closed, and a removal that still fails is logged and left
  for the next run instead of aborting. FUSE-backed shares (exFAT on Knulli and
  Batocera, NFS, SMB) keep a hidden placeholder for files unlinked while open,
  so `source-cache/bundle-*` answered `[Errno 39] Directory not empty` and a
  fully installed game was reported as a failed data setup.
- Add a regression test covering an install whose source-cache removal fails.

## 1.2.0

- Reconcile the embedded copy with the expected 1.1.2 matched-but-rejected
  candidate diagnostic and add its synthetic regression test.
- Add a generic process-scoped runtime helper that resolves native UI
  dependencies from firmware paths before inherited compatibility paths,
  removes library directories inside the game tree and preserves SDL backend
  inheritance or autodetection.
- Test the runtime boundary directly and through `run-extractor.sh` in the
  release gate.
- Add an explicit `NXEXTRACT_SDL_AUTODETECT=1` child-only recovery path for
  proven-invalid inherited SDL video/audio overrides. The default remains
  unchanged and no backend is selected by the helper.

## 1.1.2

- Make the per-launch marker check skip the full tree walk: committed trees
  are re-checked only through their anchor `required_paths`, while install,
  update, verification and adoption retain full validation.
- Add `install --force-source` for transactional upgrades from a newer source
  package while preserving the current payload until validation/publication.
- Report matched-but-rejected candidates in required-payload plan errors.
  When files match a payload's source pattern but every one fails validation
  (size, sha256, crc32 or ELF machine), the error now says so and names one of
  the rejected candidates instead of claiming the payload was not found.
- Add a synthetic regression test for the rejected-candidate diagnostic.

## 1.1.1

- Log the exact full-validation rejection for every attempted ABI when existing
  game data cannot be adopted. Validation remains strict; the additional
  diagnostic identifies the incomplete or mismatched path without requiring a
  source-package scan to fail first.
- Add a synthetic regression test for the existing-data rejection diagnostic.

## 1.1.0

- Licensed the standalone project under MIT.
- Made the UI compatibility build the default release path.
- Added an AArch64 release gate that rejects GLIBC requirements above 2.30.
- Added `elf_machine: "{abi}"` for ABI-neutral recipes and an ARMv7 fallback
  regression test.
- Added a public `nxextract --version` command and engine version in new
  installation markers.
- Added complete English documentation and standalone architecture, recipe,
  contribution, security and device-compatibility guides.
- Added sanitized real-device screenshots using only the synthetic fixture.
- Added public CI, issue forms, pull-request guidance, funding/community links
  and standalone release notes.
- Validated the Python 3.7 core, GLIBC 2.17 UI and KMSDRM flow on ArkOS.

## 1.0.0

- Initial content-driven APK/APKM/APKS/XAPK extractor.
- Loose split grouping by Android package and automatic ABI selection.
- Resumable staging, bake hooks, full validation and journaled publication.
- Crash recovery, rollback, fast markers and legal-source preservation.
- Dynamic SDL2 first-run UI for fbdev/Mali, KMSDRM and Wayland-class systems.
