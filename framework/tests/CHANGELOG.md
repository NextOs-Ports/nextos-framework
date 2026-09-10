# framework/tests changelog

## Unreleased

## 1.1.7 — 2026-09-05

- O watchdog da suíte isolada do `nxbootstrap` mantém CPU, memória, arquivo,
  processos, namespace privado e um fusível finito, mas amplia o limite de
  parede para 4096 segundos. Isso evita que contenção externa do host transforme
  uma regressão funcional já aprovada em falso timeout; os limites por processo
  e as invariantes internas continuam falhando loops e regressões reais.

## 1.1.6 — 2026-09-05

- A regressão isolada do `nxbootstrap` cobre a falha física do ArkOS em que o
  PortMaster herda `/usr/$LIB/libSDL2-2.0.so.0.3000.10`, mas o objeto não existe
  nem está mapeado: somente essa classe estreita de SDL do sistema é descartada.
- Casos mapeados continuam literais; valores privados, do adapter, não SDL,
  aninhados, ambíguos ou com outra variável continuam recusados antes do jogo.

## 1.1.5 — 2026-09-05

- A bateria canônica agora verifica, imediatamente após a auditoria da própria
  infraestrutura, todos os toolchains locais e o ID exato da imagem Docker
  fixada. Docker inativo, imagem ausente, tag redirecionada, compiler ou sysroot
  divergente falham antes das suítes pesadas; o gate M17 completo continua
  repetindo a prova dentro da fronteira ABI, sem baixar nem substituir imagem.
- A matriz classifica o novo `toolchain-preflight` como filesystem automático,
  sem sinais, hardware, rede ou acesso a device, e o teste de infraestrutura
  trava comando, arquivos de autoridade e posição como os dois primeiros gates.

## 1.1.4 — 2026-09-02

- Versão da suíte que executou a bateria final da V4 (`run-safe-gates.sh`: ALL
  PASS count=141 skipped_external=3 em `4db2fff` e em `9ade891`). A tag imutável
  `framework-tests-v1.1.3` (2026-08-20) não representa esta suíte: entre ela e a
  composição terminal entraram os gates `nxinput-padset-host`,
  `nxinput-device-input-proof`, `nxgenerator-input-proof-roteiro`,
  `nxrelease-on-device-input-proof`, o harness `test_frame_proof_fatal_race`
  (nxgl 0.3.5) e as auditorias m13/m14 atualizadas. Nenhuma tag existente foi
  movida; `1.1.4` recebe a tag nova `framework-tests-v1.1.4` no commit de
  fechamento da V4. Mudança de VERSION apenas — nenhum gate foi alterado neste
  fechamento.

## 1.1.3 — 2026-08-20

- Bind every prepared SONAME alias to its exact owning record and reject an
  alias redirected to another otherwise-valid library.
- Revalidate `DT_NEEDED` by ABI and declared search roots for environments
  that explicitly promise a complete closure, while keeping selective
  inventories selective.
- Resolve host tools from a fixed trusted system path and execute their
  canonical absolute paths, so an inherited `PATH` cannot select wrappers.
  The permanent negative environment gate covers all three refusals.
- Close the aggregate regression seal around the real checkpoint schema:
  preserve the path below `LOG_ROOT` when matching the canonical command and
  require the checkpoint snapshot to carry the same `git_head` as every gate.
  Negative fixtures cover a failed checkpoint gate, a foreign snapshot and a
  redirected checkpoint destination.

## 1.1.2 — 2026-08-20

- Harden `imagefs` and the Knulli/ArkOS/CrossMix/AmberELEC environments to the
  muOS standard: image and output outside the repository, symlinks refused
  before resolution, every image path contained under the prepared root, clean
  environment plus timeouts for `debugfs`, `mtools`, `unsquashfs`, `clang`,
  QEMU, `pkg-config` and the probes, ZIP members refused when absolute,
  escaping, duplicated or symlinked, per-file receipts with size and SHA-256,
  mandatory `DT_NEEDED` resolution and verifiers that refuse a changed,
  missing, extra or symlinked file.
- Read the Knulli resolution authority by branch instead of by loose match:
  each geometry is bound to its own board/mode test, `currentResolution` is
  parsed inside its own case branch with the swapped report order bound to the
  board that uses it, an active `display.rotate` now fails, and the pure
  policy is exercised with the parsed values rather than contract constants.
- Keep ArkOS soname claims honest: every declared soname must resolve and
  declare a matching `DT_SONAME`, while providers the image ships without
  `DT_SONAME` are reported separately. Model the dArkOS ARMHF route as
  mixed-ABI (AArch64 installer, ARMHF splash and game), matching the recorded
  physical receipt.
- Correct the TrimUI/CrossMix contract and profile: ports live under
  `/mnt/SDCARD/Data/ports`, the validated control file is the one the firmware
  executes, the exported controller database is checked by exact path, and the
  library claim separates the byte-pinned subset from the ABI audit of the
  whole directory.
- Limit the AmberELEC claim to AmberELEC, validate ALSA and Pulse as regular
  ELFs of the OpenAL ABI, prove the login profile loads its fragments and scan
  the generator and template globally for device-keyed decisions.
- Seal a regression run as a whole: `framework/tools/seal-run-manifest.py`
  parses each receipt's exact fields, revalidates and recomputes every receipt
  manifest, proves the canonical gate commands one by one, requires a single
  `git_head` and writes a sealed `ALL PASS` summary inside the aggregate.
- Add `test_device_environments_negative.py`: the containment, receipt,
  closure and seal refusals are now a reproducible gate instead of a manual
  check.

## 1.1.1 — 2026-08-20

- Add the image-backed muOS 2601.1 environment and harden it with per-file
  receipts, complete ABI-local `DT_NEEDED` closure, repository/symlink
  containment, measured target Python/SDL and sanitized probe environments.
- Keep the artifact-dependent Chrono gates classified and directly runnable,
  but remove `chrono-m21-host`, `chrono-m23-audit` and `chrono-m24-closure`
  from the automatic framework regression. They rebuild or require a frozen
  legacy Chrono ZIP and need a dedicated port session plus physical acceptance
  before reference bytes may change.
- Add a non-redistributable source contract for the official ROCKNIX
  RK3566-Specific 20260801 image, including its compressed and raw identities,
  validated GPT, FAT32 system partition and content-pinned SYSTEM SquashFS.
- Record the image-backed AArch64 userspace inventory for SDL2, Wayland, Sway,
  Mesa/Panfrost, EGL and GLES without claiming that any display, GPU, kernel
  driver, compositor or graphics context ran during host inspection.
- Query the target SDL and late-bind target EGL/GLES symbols under QEMU with a
  clean graphics environment; exercise Wayland-unavailable and provider-missing
  fallback against the unchanged nxgl implementation.
- Keep graphics selection capability-based: the firmware's own Wayland
  configuration is evidence, not a framework selector keyed by ROCKNIX,
  device brand or an inferred Mali model. No nxgl/runtime or port changed.
- Add the image-backed Knulli viewport fixture, the ArkOS/dArkOSRE multiarch
  environment (linker script, EGL/GLES sonames and KMSDRM), the TrimUI/CrossMix
  card-tree environment reusing the validated controls case, and the
  AmberELEC/P4ELEC audio and login-environment environment.

## 1.1.0 — 2026-08-20

- Correct the Spruce/Miyoo Flip profile from the obsolete "64-bit-only" model
  to the physically proven mixed-ABI topology: AArch64 NXExtract, ARMHF
  NXSplash/game, off-path interpreter, persistent chroot and separate muOS
  32/64-bit roots.
- Add a non-redistributable spruce 4.3.4 source contract and transactional,
  external PC environment preparer.
- Add a QEMU ARMHF probe that queries the real target SDL without initializing
  video; the expected compiled drivers are KMSDRM and dummy.
- Keep the standard matrix synthetic and hardware-free. No firmware binary is
  committed and no other device profile changes.

## 1.0.0 — 2026-08-16

- First versioned release of the shared test infrastructure. `framework/tests`
  holds the canonical PortMaster ZIP auditor (`audit-portmaster-zip.py`), the
  firmware profiles and matrix, and the safe-gates runner that nxrelease and
  the port build scripts shell out to. It gains a `VERSION` so the framework
  pin can snapshot it beside the other components: without it a port built
  from a materialized snapshot could render and validate a manifest but never
  bundle, because nxrelease could not find the auditor.
