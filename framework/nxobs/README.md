# NX observability and support bundles

A 0.4.4 preserva no support bundle, sanitizados e somente como OBSERVED, os
receipts de admissão C6 do nxinput 0.10.0 (`NXC6-SEAM`/`NXC6-DOMAIN`):
autoridade vencedora, motivos por degrau, classificação de domínio,
aquisição do banco vivo e FACE_LAYOUT — sem pid/tid/timestamps, prosa ou
campos não listados.

Versão atual: **0.4.4**.

A 0.4.3 torna o diretório remoto de prova visual privado e exclusivo antes de
iniciar o port. O harness usa `umask 077` com um único `mkdir`; colisão com
diretório ou symlink falha fechado e o cleanup só remove o caminho cuja criação
esta invocação confirmou. Não há `mkdir -p`, correção posterior com `chmod` nem
dependência do comando externo `stat`.

A 0.4.2 preserva no support bundle, sanitizados e somente como OBSERVED, os
receipts de observabilidade de input do nxinput 0.7.0 (LOAD/CAPABILITIES/
BINDING/CHORD/EVENT/CONSUMER) — sem promover nenhum estado.

A 0.4.1 faz o support bundle preservar, sanitizado, o receipt final
`GRAPHICS-EVIDENCE` do nxgl (V4-GRAPHICS-04) e o diagnóstico pré-present como
evento apenas observado — sem caminhos de provider e sem jamais promover
`awaiting-first-present`.

A 0.4.0 acrescenta `nxobs_perf`: amostrador passivo, somente leitura e limitado
por construção, com threads, descritores abertos, CPU, `/proc/self/io`,
RSS/HWM, os quatro intervalos de boot especificados e o recibo VSYNC com
intervalo **pedido** e **efetivo** separados mais o FPS medido. Observar nunca
autoriza agir: o módulo não mata processo, não limita save e não estrangula
frame. O caminho de frame é O(1), sem I/O e sem log — spam por frame reprova QA
e leitura de `/proc` por frame desgasta o cartão. Campo ausente vira máscara de
completude, nunca zero silencioso. É aditivo: nada roda enquanto o adapter não
chamar, e nenhum port aprovado é migrado.

Versão anterior: **0.4.2**.

`nx-support-bundle.py` correlates an `nxbootstrap` runtime log with the separate
NXExtract log under one bounded support run ID. It never copies either raw log.
Instead it emits a versioned JSONL event stream, a sanitized report, a short
human summary and a SHA-256 manifest.

The live runtime remains the authority: explicit `NXEVENT` records retain their
monotonic timestamps and durations. Legacy/plain markers are classified in
their observed order and keep timing fields `null`; the tool does not invent
durations. `NXCOMPAT_REPORT` data is reduced to finite capability IDs, states,
reason codes and bounded receipts. Device names, paths, addresses, hostnames,
credentials and save data never enter the bundle. Credential-like structured
fields are replaced by finite class markers. Embedded `Authorization`/Bearer,
API key, authentication header, cookie, token, key, session and auth syntax in
plain text, serialized JSON or query strings follows the same fail-closed path;
the value is never retained. Non-sensitive phase, status, reason code and
bounded details remain available for diagnosis.

Inputs must be regular non-symlink files no larger than 8 MiB, each line is
bounded to 64 KiB, and at most 2,048 events are emitted. The destination must be
a new absolute path under a real directory. All files are written exclusively
to a private temporary directory and renamed only after the manifest exists;
failure leaves no partial bundle. Distinct outputs are independent and an
existing destination is never overwritten.

Example (internal raw paths deliberately omitted):

```sh
python3 -B framework/nxobs/nx-support-bundle.py \
  --runtime-log RUNTIME_LOG --extractor-log NXEXTRACT_LOG \
  --artifact chrono.zip --stack-id arkos-rk3326-kmsdrm \
  --firmware-context arkos --output /absolute/new/support-bundle
```

Raw logs remain internal. Only the output directory created by
`nx-support-bundle.py` (or its manifest hash) may be attached to a public issue.
Never attach `log.txt`, rotated logs, `nxextract.log`, the source
`events.jsonl`, crash maps or any other raw input.

`tools/nx-logs IP SLUG` is the public collection path. By default (and with the
legacy `--bundle` spelling) it receives raw inputs into a private `0700`
staging directory, validates the TAR before extraction, runs this sanitizer,
publishes only the five-file bundle and deletes staging on every path. A
sanitizer or archive failure publishes nothing. `--raw-internal` is the sole
raw-evidence escape hatch; its output carries an `INTERNAL-ONLY-DO-NOT-SHARE`
marker and is never a public support artifact.

## V3: eventos nativos e budgets

Desde 0.3.0, `nx-support-bundle.py --events-file` consome o `events.jsonl`
`nx-event-v1` produzido pelo runtime, mantendo o mesmo `run_id` entre launcher,
NXExtract, loader e jogo. Uma linha final interrompida recebe diagnóstico
explícito; uma linha inválida no meio falha fechado. As famílias NXU, NXA, NXG,
NXI, NXO, NXR e NXE são finitas e versionadas.

`nx-budget-check.py` emite `nx-budget-receipt-v1` para os limites de logs e
eventos. Esse gate mede somente artefatos controlados pelo framework: não mata
jogo, não limita save e não transforma os tetos iniciais em promessa universal
de performance.

O escopo de evidência V3 permanece host/sintético para budgets e telemetria. Um
receipt físico de um port prova apenas aquele artefato e stack; não promove um
budget universal por inferência.

## Telemetria passiva de memória (`nxobs_mem`, P5)

`include/nxobs_mem.h` + `src/nxobs_mem.c` são o amostrador opt-in de pressão
de memória para adapters: leitura pura de `/proc/self/status` e
`/proc/meminfo` (`VmRSS`, `VmHWM`, `RssAnon`, `RssFile`, `VmSwap`,
`MemAvailable`, `SwapTotal`, `SwapFree`) com piso de intervalo de parede de um
segundo — o adapter chama o tick a cada frame e o custo continua limitado. O
adapter pode registrar callbacks opacos de heap (ex.: `il2cpp_gc_get_*`) e um
marcador curto por amostra; o framework não interpreta nenhum dos dois, não
conhece classes nem offsets de jogo e nunca decide por nome de aparelho.

Campo que o kernel não expõe vira `status=incompleto`/`meminfo=incompleto` na
linha — os dois kernels de campo dos handhelds (3.14 e 4.4) antecedem
`RssAnon`/`RssFile` e o log diz isso em vez de mentir zero.

**Observar nunca autoriza agir.** Este módulo não faz GC, unload, descarte de
textura nem `malloc_trim`. Evidência negativa registrada no P5: GC IL2CPP
periódico já produziu `SIGSEGV` em campo, e limpeza quente de texturas deixou
o mundo preto no Bully. Qualquer ação de memória permanece port-specific,
experimental e presa ao gate físico próprio (Wave 20 em 1 GB + regressão em
2 GB) antes de qualquer promoção.

Gate hermético: `tests/run-mem-sampler.sh` (fixtures sintéticas de /proc,
piso/supressão de frequência, sanitização de marcador, máscaras e callbacks).

## Native crash receipt

`include/nxobs_crash.h` and `src/nxobs_crash.c` provide an opt-in native crash
boundary for project-built adapters. It observes `SIGSEGV`, `SIGABRT`,
`SIGBUS`, `SIGILL` and `SIGFPE` without replacing the game's lifecycle. The
handler writes one bounded `nx-crash-v1` JSON record, calls `fsync`, and
re-raises the original signal with `SA_RESETHAND`; the real signal/status remain
visible to the launcher and PortMaster.

The adapter must pass a private, owner-only, non-symlink runtime directory. The
component creates exclusive mode-`0600` files named
`<port-id>-crash.<pid>.jsonl` and `<port-id>-maps.<pid>.txt`. A normal uninstall
of the observer removes both. A crash preserves:

- signal/status, current phase and frame;
- thread, PC, LR and SP when exposed by the architecture's `ucontext`;
- fault address, module basename, GNU build-id and module-relative offset;
- the last bounded asset ID, EGL/GL call ID and selected provider;
- a sanitized module map containing ranges, basenames, load bias and build-id,
  never process paths.

State setters copy into double-buffered bounded slots outside the handler. Call
them at already-existing lifecycle boundaries; do not add a second render call,
skip native initialization, reorder draw, swallow the signal or keep the game
alive. The source compiles for AArch64 and has explicit register paths for
AArch64, ARMv7 and x86_64.

Minimal integration:

```c
#include "nxobs_crash.h"

struct nxobs_crash_config crash = {
    .runtime_dir = private_runtime_dir,
    .port_id = "swordigo",
    .initial_phase = "native-init",
    .initial_provider = "wayland-gles2",
};
if (nxobs_crash_install(&crash) == 0) {
    nxobs_crash_set_phase("draw");
    nxobs_crash_set_frame(frame_index);
    nxobs_crash_set_last_asset("ferryman-atlas");
    nxobs_crash_set_last_graphics_call("glDrawElements");
}
```

`nx-support-bundle.py --crash-receipt RECEIPT` imports up to eight receipts,
validates the exact schema and records only bounded sanitized fields plus source
hashes. A failed crash event is terminal even if a stale successful shutdown
line exists elsewhere in the input logs.

## Private symbols and offline symbolization

Project-built release binaries must retain an unstripped private counterpart.
Archive it outside every public ZIP:

```sh
python3 -B framework/nxobs/nx-archive-symbols.py \
  --private-root /absolute/private/mode-0700 \
  --output /absolute/private/mode-0700/swordigo-1.4.13 \
  --binary build-unstripped/swordigo-nextos \
  --source-commit COMMIT_SHA
```

The archiver requires a GNU build-id, copies each binary mode `0600`, emits a
hash manifest with `public_zip_member=false`, and publishes the new directory
with `renameat2(RENAME_NOREPLACE)`. It cannot target a path outside the declared
private root.

Resolve a receipt only against the exact build-id:

```sh
python3 -B framework/nxobs/nx-symbolize-crash.py \
  --receipt CRASH.jsonl \
  --binary PRIVATE_UNSTRIPPED_BINARY \
  --output /absolute/new/symbolized.json
```

The output contains function, source basename and line, never the DWARF source
path. A basename or build-id mismatch fails closed.

## Compatibility and regression boundary

The 0.1.0 support-bundle CLI and `nx-event-v1` remain accepted. Crash ingestion
is additive and absent by default. Version 0.3.0 adds native `events.jsonl` and
bounded log-budget receipts. Version 0.3.1 changes only the public sanitization
boundary: credential classes are redacted through deterministic markers and
credential-like public metadata is rejected. This component renders no UI,
opens no DRM, EGL, audio or input device, and never executes a packaged game.
Host gates cover early/late `SIGSEGV`, `SIGABRT`, signal preservation, private
permissions, symlink/unwritable roots, AArch64 compilation, build-id
symbolization, private symbol archival, adversarial crash JSON and synthetic
credential leakage. No version changes a visual interface, renderer or
fallback. In particular, nxobs 0.3.1 changes no visual interface, renderer or
fallback behavior.

## Device launch harness

`nx-device-launch.sh` starts a port on a device the way that device's own
frontend starts it, and exits with the verdict rather than a description.

```sh
bash framework/nxobs/nx-device-launch.sh \
  --host <current device IP> --user <user> [--password <pw>] \
  --launcher "/roms/ports/<Name>.sh" [--seconds 40] [--keep-running]
```

It discovers the frontend unit on the device, reads the `Environment=` lines
that unit declares and reproduces them under `env -i`. This matters: on
ArkOS/dArkOS the frontend unit carries
`Environment="SDL_VIDEO_EGL_DRIVER=libEGL.so"`, because the firmware's
versioned SONAMEs (`libEGL.so.1`, `libGLESv1_CM.so.1`) resolve to driverless
stubs while the unversioned names are the real Mali blob. A port launched from
a plain SSH shell does not inherit it, `SDL_CreateWindow` fails, the GL
provider repair takes its pre-context branch, every candidate reports
`eglInitialize failed on this kernel`, and the run looks exactly like a broken
port. Nothing is hardcoded, so a firmware that declares something else is
handled the same way.

Exit status is the verdict: `0` the port drew a real frame, `1` it measured
black on a launch that could have drawn, `2` nothing was proven. An
inconclusive run is never reported as a defect. The frontend is restored on
every exit path, interrupts included.

Before the port starts, the harness claims a fresh `/tmp/nx-proof-<pid>` with
mode `0700`. An existing directory or symlink aborts the launch and is left
untouched; cleanup is restricted to a path created by that invocation.

`--keep-running` leaves the game up with no timeout for hands-on play.
