## 1.3.0
- **Os orçamentos de log viraram limite de verdade.** `nx-budget-check`
  declarava 2 MiB para o log compacto e 8 MiB (+1 rotação) para o de detalhe, e
  **nada aplicava nenhum dos dois**. O NXExtract roda na hora da instalação, no
  mesmo cartão em que os payloads vão cair: um log sem teto disputa espaço com
  a própria extração que ele está narrando. Agora o log compacto — que é a
  narrativa de abertura (identidade da receita, descoberta, plano) — para no
  teto **dizendo na última linha que parou**, porque um corte silencioso é
  indistinguível de uma execução que morreu; e o log de detalhe, que é o fluxo
  sem perdas, **rotaciona uma vez**, então a janela mais recente sobrevive
  inteira e exatamente uma geração anterior é guardada. Uma rotação que o
  filesystem recuse não derruba o fluxo: continua escrevendo onde já estava.
  Os tetos estão fixados no teste contra os números publicados, e verificados
  por mutação: não limitar o compacto, não rotacionar o detalhe e limitar sem
  avisar reprovam o gate, um a um.


- **A cerca de recursos dos hooks de receita falha fechada.** Até a 1.2.21
  qualquer erro ao estabelecer `RLIMIT_CPU`, `RLIMIT_AS`, `RLIMIT_FSIZE` ou
  `RLIMIT_NPROC` era engolido, e um host onde o limite não podia ser aplicado
  rodava o hook **sem teto nenhum** — exatamente o estado que esses limites
  existem para impedir. Agora o erro sobe dentro do filho antes do `exec`, o
  hook nunca começa sem cerca e o motor reporta `cannot start hook <id>`. A
  regressão tem controle positivo: a mesma receita roda o hook normalmente sem
  a falha injetada, então o negativo não pode passar de forma vazia.
- **Negativo novo do grupo de processos.** Um hook que cria filhos e ignora
  TERM leva os filhos junto: matar só o hook deixaria os filhos rodando depois
  da extração, escrevendo no cartão muito tempo depois de o motor ter
  reportado falha — exatamente o órfão que o kill por grupo existe para
  impedir. O teste espera além do instante em que o filho escreveria seu
  marcador e exige que ele nunca apareça; trocar o `killpg` por `kill` reprova
  o teste.
- Nada disso se aplica ao processo do jogo; nunca se aplicou e continua não se
  aplicando. Limites, tetos, relógio de parede e orçamentos de linha e de saída
  total permanecem iguais.
- **Compatibilidade de APK passa a ser provada contra uma cópia legal real.** O
  gate novo do framework roda o **pipeline gerado**, não funções isoladas,
  contra uma cópia fornecida pelo dono, em variantes renomeada, reempacotada
  (ordem invertida, `stored`, timestamps futuros, atributos DOS e extra fields
  estranhos) e em outro formato de container suportado — todas têm de resolver
  o mesmo conjunto de payloads. Os negativos são package de outro jogo, payload
  obrigatório ausente e ABI não declarada. O artefato proprietário nunca entra
  no repositório e os recibos guardam só identidade técnica.
- Runner e runtime-env continuam byte-idênticos à 1.2.21, e a apresentação não
  muda: UI SDL2, renderer fbdev, goldens 640x480 e 1280x720, schema do
  terminal-result e os quatro binários imutáveis de UI são os mesmos.
- Nenhum port publicado é migrado: ele continua válido no motor que embarca, e
  a identidade de saída 1.2.21 fica registrada no ledger de motores no momento
  deste bump.

## 1.2.21

- Mantém o selo barato de metadados no caminho normal, mas adiciona um selo
  estável e forte de conteúdo para autenticar payloads quando FAT, exFAT ou FUSE
  muda/quantiza `mtime`. Metadado divergente com bytes idênticos atualiza apenas
  o marcador; alteração de conteúdo do mesmo tamanho continua reprovando.
- Migra de forma limitada marcadores 1.2.20 após validação completa da receita e
  sela o conteúdo instalado sem copiar nem reextrair dados. Se o selo legado de
  metadados divergir, exige um selo forte esperado independente. Versões mais
  antigas continuam falhando fechadas.
- Adiciona `--reuse-only` e `NXEXTRACT_REUSE_ONLY=1`: o fluxo aceita/migra os
  dados existentes ou aborta antes da UI, da busca de fonte e da extração. Isso
  permite testar uma atualização de binário sem risco de iniciar uma reinstalação
  demorada. Uma transação pendente também reprova sem executar recuperação,
  rollback ou roll-forward implicitamente.
- A identidade gráfica, a exigência de renderer visível em instalações reais, o
  protocolo da UI e seus quatro ELFs permanecem byte a byte inalterados.

## 1.2.20

- Correção antes de tag/publicação: os papéis de `required_members` agora têm
  semântica runtime real sobre base+splits. `core_required` falha se ausente,
  `optional` só registra presença, `variant_required` acompanha o ABI resolvido
  e `patch_selector` autentica o payload interno por SHA-256. Um match único
  escolhe o perfil; ausente/desconhecido usa o fallback comum sem rejeitar.
- A decisão autenticada (`org.nextos.nxextract.compatibility-result/1`) entra no
  fingerprint do plano, invalida checkpoint de hook de outro perfil, é entregue
  em `NXEXTRACT_COMPATIBILITY_JSON` (variável reservada) e fica no marker com
  fingerprint próprio. Adoção sem source registra explicitamente `mode=existing`
  e não inventa seleção de patch.
- Fecha bypasses por `source_validate`/`output_validate`,
  `container.source.patterns` literal,
  membros de assinatura/certificado em `META-INF` e fonte local empacotada de
  hook. Testes reais: APK/APKM/APKS/XAPK, Terraria `.4/.49`, Off The Road com
  fixture sintética nomeada `arm64-2/roles`: instala sem `AVConfig.json`, exige
  `libgame.so` e `libc++_shared.so` AArch64 mais `data_001.xpk`, aceita container
  renomeado/reempacotado e rejeita package/ABI/ELF/dados divergentes. Também
  cobre fallback presente/desconhecido/ausente, ambiguidade e resume. UI,
  runner e runtime-env permanecem byte-idênticos.
- APK-COMPAT item 9: identidade cosmética do container (assinatura/certificado
  NXA0004, filename NXA0005, versionCode exato NXA0006) nunca decide
  compatibilidade; papéis core_required/optional/variant_required/patch_selector
  em required_members (NXA0026..NXA0029); fingerprint SHA-1 no scan estático
  (NXA0054). apkcompat.py embutido byte-a-byte (sync-gated). Runner e runtime-env
  idênticos ao 1.2.19; 1.2.19 permanece motor suportado no registro. Suíte: 99.

## 1.2.19

- APK-COMPAT-01 (V3): container identity never decides compatibility. The
  canonical shared module `framework/contracts/apkcompat/apkcompat.py` is
  embedded verbatim (sync-gated); container rules refuse `sha256`/`crc32`
  whitelists of ANY length and exact `size` (NXA0001..NXA0003), closing the
  1.2.18 loophole that accepted lists of two or more digests.
- New optional recipe blocks separate identity from decision:
  `reference_build` (documentation only), `compatibility` (cross-checked
  against `input.packages`/`abi_order`) and `patch_profiles`
  (internal-payload match with mandatory generic fallback).
- Hooks may declare an inline `contract`
  (org.nextos.apk-compat.hook-contract/1); predicates are classified
  reference_identity/compatibility/patch_selection, identity-only rejection
  is refused (NXA0046), and the `NXEXTRACT_PREDICATE` stdout protocol names
  the failing predicate in the terminal error.
- Static defence `scan_static_suspects` (NXA0050..NXA0053) flags 64-hex
  equality, literal dotted version tokens, size tables and absolute offsets
  for the generator/release gates. Terraria `.4` vs `.49` is a pinned
  regression.
- V3-OBS-01: with `NXOBS_RUN_ID`/`NXOBS_EVENTS_FILE` from the launcher, the
  engine appends `nx-event-v1` lines to the shared `events.jsonl` (best
  effort, additive).
- V3-HARDENING-01: recipe hooks run inside a resource fence (own process
  group; RLIMIT_CPU/AS/FSIZE/NPROC; wall timeout; 64 KiB line and bounded
  total output caps, recipe-tunable DOWN through `hook.limits`); a violating
  hook is killed by group, the validated stage survives, and the game process
  is never fenced. Recipes gain a 1 MiB parse ceiling.
  No UI, pixel, flow or schema change; suite: 89 cases.

## 1.2.18

- Every terminal error sink is now redacted: the setup-screen failure text
  (`progress.fail`) and the `TERMINAL ERROR` log line pass through the same
  `sanitize_terminal_message` filter as the terminal JSON. A raw exception can
  no longer leak a host path or the owner's container filename into the log a
  player publishes (the redaction test now covers all three sinks).
- The "no container found" error finally tells the player what to do: it names
  the `gamedata/` folder (create it if missing) and lists the accepted
  extensions (.apk .apkm .apks .xapk .zip .obb). This is the single most
  common BYO-data support case.
- Nothing else changes: setup screen pixels, the separately pinned immutable
  1.2.16 UI, the terminal-result schema and the NXE#### format are unchanged.

## 1.2.17

- Preparation hooks can now be transactional, by opt-in, through the additive
  boolean hook member `transactional`. The hook writes every output into a
  shadow workspace provided by the engine (`NXEXTRACT_HOOK_SHADOW`) and never
  touches the validated stage inputs; the engine proves those inputs unchanged
  (SHA-256 before/after), validates the checkpoint on the shadow+stage
  overlay, seals a journal with per-output SHA-256, counters and an integral
  fingerprint, and only then publishes — one atomic rename per target, with
  `fsync`. A preparator interrupted between targets can no longer leave the
  stage as a mix of original and transformed files (P6 items 1–5).
- Interruption before the seal discards the temporaries, so a retry starts
  from the same pristine inputs. Interruption after the seal rolls forward
  deterministically from the journal; a journal whose shadow was already
  consumed — the resumed extraction restores replaced targets from the source
  — is discarded and the hook reruns over pristine data. A partially prepared
  stage is never adopted.
- Transactional hooks run unbuffered (`PYTHONUNBUFFERED=1`) and a hook failure
  now carries the last detail line, sanitized (no absolute paths, no owner
  container names), into the summary and the terminal error.
- Covered by `tests/test_hook_transaction.py`, which runs the real extractor
  and injects `SIGKILL` after each target, a simulated `ENOSPC` and a
  validation failure: the retry of the same container ends at the same
  fingerprint, with no `.nxpart`, no extra target and no change to the
  previously published live payload.
- Hooks without the member behave byte-identically to before. The setup
  screen, the separately pinned immutable 1.2.16 UI, the terminal-result
  schema and the NXE#### format are unchanged.

## 1.2.16

- The private UI session moved from a `0700` runtime directory to inherited
  descriptors. The engine opens two pipes before the spawn, validates owner,
  FIFO type, private mode and exact dev/inode identity of every descriptor
  (fail closed), and passes `fd:N` tokens in the historical `stop`/`ready`
  argv slots. The readiness proof is sealed by the UI closing its write end;
  the stop request is a byte plus EOF, so an engine crash ends the UI instead
  of orphaning it. No `ui.ready`/`ui.stop` pathname exists anywhere anymore.
- Field failure fixed (P1): on firmwares with `Linger=no` — dArkOSRE measured —
  systemd recycles `/run/user/<uid>` when the login session ends, and a long
  extraction that kept its handshake under `XDG_RUNTIME_DIR` died with
  `NXE7001: UI runtime directory identity was lost` after copying everything.
  With the session channel in descriptors, deleting any directory mid-copy —
  the exported `XDG_RUNTIME_DIR` included, which stays untouched for Wayland —
  no longer invalidates a data transaction that already validated. Covered by
  a long-extraction fixture that recycles the session mid-copy and by
  negative fixtures for substituted, foreign-owned, wrong-mode and non-FIFO
  descriptors.
- The UI binary understands the `fd:` tokens, polls the stop descriptor and
  still accepts the historical absolute-path contract, so it keeps working
  with older engines. Pixels, text, ordering and duration are unchanged; the
  visual-identity goldens hold byte for byte.
- `NXEXTRACT_REQUIRE_VISIBLE_UI=1`, the 40-second fail-closed boundary, the
  approved-renderer proof set and the headless fallback semantics are
  unchanged.

## 1.2.15

- Added `source_validate` and `output_validate` to an `extract` item, splitting
  the rule checked on the freshly extracted payload from the rule checked on
  the final result after hooks. Until now the same `validate` was used for both
  phases, so a hook that legitimately changes file sizes forced the recipe to
  widen into a range covering both states — and a widened range also accepts a
  stage interrupted mid-transformation, which is the state it should reject.
  With the phases separated each side can declare exact size and fingerprint,
  and the intermediate state satisfies neither. Purely additive: an item that
  declares only `validate` behaves exactly as before.

## 1.2.14

- P13: o `NXE6001` historico passa a se dividir por `errno` -- cartao cheio
  (`NXE6002`), permissao/somente-leitura (`NXE6003`), caminho ausente
  (`NXE6004`) e midia removida/erro de I/O (`NXE6005`). A TELA nao muda: o
  codigo ocupa o mesmo lugar, mantem o formato `NXE####` que os launchers
  publicados ja validam e nenhuma categoria nova de resultado aparece.
  `errno` fora do mapa continua `NXE6001`.

## 1.2.12

- P12: membro opcional `mutable` no recipe — caminhos sob `commit` que o
  GUEST pode criar/alterar (saves gravados junto dos assets) ficam fora do
  selo de metadados; fecha o loop "toda abertura mostra a setup UI" do
  Tightrope. Recipes sem o membro = comportamento byte-identico.

## 1.2.11

- P11.6: quando a setup UI nao abre um renderer, o motivo (ultima linha do log
  da propria UI, ex. o erro SDL) entra no resumo -- o NXE-diagnostico nomeia a
  causa sem segunda viagem.
- P11.7 (fallback HEADLESS): a instalacao do usuario NUNCA mais morre porque a
  UI visual nao abriu (caso de campo NXE7001 no muOS/RG35XX-H). Sem renderer
  apos as tentativas, a extracao segue headless -- a UI e so a barra de
  progresso -- e o NXEXTRACT_RESULT registra `ui.mode=headless-fallback` +
  `ui.fallback_reason`. Com renderer disponivel, NADA muda (UI visivel padrao).
  O gate de release/QA restaura o fail-closed com NXEXTRACT_REQUIRE_VISIBLE_UI=1.
- Novo campo obrigatorio `ui` no terminal-result (schema atualizado):
  {mode: visible|headless-fallback|disabled, renderer, fallback_reason}.

# Changelog

## 1.2.13

- Preserve the approved setup renderer as historical terminal evidence after
  the UI process and its private runtime controls are cleaned up. A successful
  visible run now reliably publishes `ui.mode=visible` with `renderer=sdl` or
  `renderer=fbdev`; headless and disabled receipts keep their prior meaning.
- Keep the graphical UI source, four release binaries, pixel goldens,
  transaction flow and terminal-result schema version byte-for-byte unchanged.

## 1.2.10

- Publish `nxextract-result.json` as an atomic, versioned terminal result with
  final phase, success/error outcome, stable support code, recipe identity,
  package ID, ABI, validated counts/bytes, critical payload summaries and
  relative log paths. Container identity is derived from validated content and
  never includes the external filename, URL or download origin.
- Split logging into a compact milestone log and the lossless
  `nxextract-detail.log`. Per-file extraction and hook output stay in detail by
  default; `--verbose-log` or `NXEXTRACT_VERBOSE_LOG=1` restores them to the
  compact stream for diagnostics.
- Keep the first miss in each class visible, summarize repetitions and always
  retain the terminal cause and stable `NXE####` code in the compact log.
- Seal source kind and package ID into new installation markers, so the
  source-independent fast path emits the same sanitized terminal identity.
  Existing-data adoption now records every validated file in tree payloads.
- Add atomic-publication failure, install/fast/adoption/error, redaction and
  2,000-file compact-log regressions. The approved SDL/fbdev renderer source,
  640x480 and 1280x720 pixel goldens, and all release UI ELFs are unchanged.

## 1.2.9

- Reuse the portable EGL/GLES provider recovery physically validated by
  NXSplash 0.1.2. After a normal SDL window attempt fails, KMSDRM can retry
  `libEGL.so`/`libGLESv2.so` immediately; explicit firmware or user provider
  choices remain authoritative.
- Raise the fail-closed graphical readiness deadline from 20 to 40 seconds.
  This covers the complete bounded SDL retry budget plus graphical fbdev on
  slow ArkOS-class stacks without allowing extraction to run headlessly.
- Add a simulated deadline-boundary regression proving that readiness at
  20.5 seconds succeeds while no proof at 40 seconds still fails, plus a fake
  SDL provider gate for portable recovery and explicit-provider preservation.
- Require `input.packages` for `container` recipes and reject a whole APK
  locked to one SHA-256, one CRC32 or one exact size. Preserve both published
  compatibility forms: two or more explicit official container identities,
  and hash-free containers qualified by package plus internal payload/tree/hook
  validation.
- Keep `draw_screen()`, both 640x480 and 1280x720 pixel goldens, transaction
  logic, content-driven APK discovery and all internal payload validators
  unchanged.
- Ship the bundle-pin helper inside the standalone component tree. An exported
  source archive now runs its full release gate without reaching into the
  parent monorepo.
- Keep the graphical readiness/control plane off FAT/exFAT/FUSE game trees.
  `ui.ready` and `ui.stop` now live in a unique owner-private runtime
  directory selected from a safe `XDG_RUNTIME_DIR` or `TMPDIR`/`/tmp`; mode,
  owner, inode and symlink checks remain fail-closed and cleanup is guaranteed;
  the non-authoritative diagnostic `ui.log` remains persistent in the workspace.
- Add a chmod-insensitive `0777` game-tree fixture, unsafe-environment fallback
  coverage and an all-unsafe fail-closed fixture. Only the private runtime proof
  authorizes extraction; an unusable `XDG_RUNTIME_DIR` or `TMPDIR` falls back to
  `/tmp`, while no unsafe base is returned as a control directory.

## 1.2.8

- Restore the proven 1.2.6 graphical negotiation budget: six attempts per
  advertised SDL backend, six inherited-backend attempts and 500 ms between
  retries. A transient KMSDRM/Mali provider failure can no longer demote a
  working device to the console after a single 200 ms attempt.
- Preserve the approved SDL panel, palette, typography and progress bars, and
  reuse that exact renderer on a direct framebuffer software surface when a
  firmware exposes `/dev/fb0` but cannot create EGL/GLES.
- Keep the graphical framebuffer renderer working on drivers that reject
  `mmap` but provide bounded reads/writes, using the same frame with row-safe
  writeback rather than changing the interface or accepting TTY readiness.
- Quarantine the 1.2.7 ASCII/TTY renderer behind an explicit diagnostic opt-in.
  Public `--require-ui` runs accept only an exact private `visible=sdl` or
  `visible=fbdev` proof after the first complete graphical frame is presented.
- Extend the readiness deadline to the bounded 20 seconds required by the
  restored negotiation budget. An unsafe, malformed, TTY or dead-renderer proof
  still fails before package discovery or owner-data mutation.
- Expand the synthetic suite to 61 cases with separate SDL/fbdev acceptance and
  an explicit regression proving that TTY readiness cannot authorize extraction.
- Add deterministic, manifest-pinned AArch64, ARMv7, x86_64 and i386 release
  artifacts. This closes the path that could vendor an AArch64 UI into an
  ARMv7 port while keeping every UI at GLIBC 2.17 or older.

## 1.2.7

- Add a bilingual active-VT terminal renderer for systems whose advertised SDL
  backend exists but cannot create a real EGL/GLES window. SDL remains first;
  only the kernel-published active `tty<N>` is used after genuine failure.
- Make visible setup an attested precondition of the packaged runner. The UI
  writes a private readiness proof only after SDL or TTY output opens, and
  extraction fails before scanning owner data when that proof is absent.
- Reduce failed SDL retries to one capability-driven attempt per advertised
  backend, so terminal progress appears promptly instead of spending most of
  extraction retrying the same unavailable graphics stack.
- Make the complete five-file kit mandatory: engine, runner, runtime helper,
  recipe and the GLIBC 2.17 UI are all pinned. The synthetic suite now has 59
  cases, including visible-readiness success, dead-renderer rejection and
  fail-before-extraction.
- Pin the compatibility build to the Debian Buster image used by the public
  low-glibc toolchain; the final AArch64 ELF remains PIE/RELRO/NOW with only
  `libc.so.6` and `libdl.so.2` dependencies.

## 1.2.6

- Case-insensitive ZIP path collisions are enforced over the members a recipe
  actually selects, not over the whole archive. Real Android APKs routinely
  carry obfuscated resources that differ only in case (`res/9N.9.png` vs
  `res/9n.9.png`); refusing the archive rejected legitimate builds whose
  colliding members are never extracted. The extracted tree is still
  guaranteed writable on exFAT/FAT cards, because the check now covers exactly
  what gets written.

All notable NXExtract changes are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 1.2.5

- Introduce transaction journal format 2. Every backup/install rename now has
  a durable write-ahead intent, the rename fsyncs both parent directories, and
  the confirmed state is fsynced afterward. Fourteen simulated power-loss
  boundaries prove that recovery either restores the complete previous payload
  or retains the complete newly published payload.
- Make the marker a strict publication record: exact engine/recipe/ABI/commit
  identity, plan fingerprint, transaction ID, item schema and a metadata seal
  of every committed object. Recovery performs full payload validation before
  trusting even a matching marker and rolls back corrupted publication.
- Reject recipe parents, workspace objects, logs, locks, journals, stages,
  caches and hook checkpoints that are symlinked or hard-linked. Recipe
  templates and reserved path overlaps now fail before log/workspace effects.
- Reject ZIP traversal, special/encrypted members, file/directory conflicts and
  Unicode/case-fold path collisions globally, including members a recipe would
  not otherwise select. Published trees also reject symlinks, hardlinks and
  non-portable collisions.
- Seal cache-space accounting to valid cached inner APKs only; stale or extra
  cache files cannot hide the actual missing-space preflight.
- Add exact whole-bundle pinning for new/touched ports. Engine, runner, runtime
  helper and optional UI are compared by SHA-256; 1.2.2 remains only the
  documented historical audit floor.
- Audit every ELF found in the release tree. The AArch64 UI must be PIE with
  RELRO/NOW, non-executable stack, no RPATH/RUNPATH, exact AArch64 interpreter,
  only `libc`/`libdl` dependencies and GLIBC no newer than 2.30. The current
  unchanged UI requires GLIBC 2.17.
- `run-extractor.sh` now invokes the runtime helper explicitly through `bash`,
  so a PortMaster ZIP installed on FAT/exFAT remains usable when executable
  mode bits are lost. The helper still owns the process-scoped library and SDL
  isolation before the launcher re-enters itself.
- Once the validated installation marker is published, cleanup of the
  transaction backup, stage, source cache or journal can no longer turn the
  install into a failure. Refused cleanup keeps the journal as a retry signal;
  the next run recognizes the marker's transaction and safely finishes it.
- Expand the release suite to 55 synthetic Python tests plus isolated runtime,
  full-bundle pin and hostile-ELF gate regressions. No APK, OBB, ZIP or
  proprietary game data is included in the source release.

## 1.2.4

- New `input.packages`: a recipe can declare which Android package it accepts.
  Two games from the same studio share the engine, the asset names and the
  layout, so content rules alone let the wrong one install silently; the refusal
  now names both the package that arrived and the one the port expects.
- New source kind `container`: copies the selected APK itself as a single file.
  Ports whose engine reads its resources from the package at run time (Cocos2d-x
  with minizip, for example) could only be served by extracting the entire asset
  tree and repacking it, which needs twice the card space during install and
  leaves both copies committed. `container` picks the base APK of the set by
  default, or a named `split`, and never considers the bundle that wraps it.

## 1.2.3

- Zip-format OBB files (Aspyr, Gameloft, Rockstar and others publish OBBs that
  are plain ZIP archives) are now also kept as loose-file candidates during
  discovery. Before this, a loose `.obb` that happened to be a ZIP was
  classified only as a companion archive, so `file`/`entry_or_file` rules that
  select the OBB itself could never match it — the installer searched inside
  the OBB instead of taking it as the payload. `entry` rules that look inside
  such OBBs keep working unchanged. First hit: KOTOR (build 53), whose two
  OBBs are ZIPs and are consumed whole by the game's own ObbFile layer.

## 1.2.2

- Discard staged data that fails whole-set validation instead of keeping it for
  the next resume. Each extracted item is accepted on its own, so a stage that
  only fails as a *set* used to be resumed and re-rejected forever: the field
  log showed `resuming 1.2 GiB of already validated staged data` followed by the
  same validation error on every retry, with no way out short of deleting the
  workspace by hand. The stage and `state.json` are now removed when whole-set
  validation raises, so the next run extracts from scratch. Found in the field
  on Hitman GO 1.1.1 and carried as a local patch there; this promotes it
  upstream so ports vendor a clean tree.

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
