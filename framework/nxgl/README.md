# nxgl

`nxgl` is the common, static SDL2/GLES window-and-context layer for new
NextOS/PortMaster compatibility loaders. Its real baseline is **Mali-450 with
OpenGL ES 2.0**. It does not require Mesa, desktop OpenGL, or GLES 3.x.

The library solves the part that is genuinely common between games:

- preserve the video backend inherited from PortMaster/the firmware;
- consider video usable only after a real window, GLES context, delivered
  config, and drawable all succeed;
- after that real failure, remove an inherited video hint and let SDL
  autodetect once; never choose a replacement `SDL_VIDEODRIVER` by name;
- select a native window size from SDL first, then passed DRM/fbdev facts;
- try the exact GLES/RGBA/depth/stencil ladder declared by the engine adapter;
- reject a desktop-GL context before the game can render black GLSL-ES output;
- record the backend, vendor, renderer, GL/GLSL versions, real drawable, and
  delivered config;
- leave presentation and every engine lifecycle call under explicit ownership.

`nxgl` is not an EGL emulation layer and does not invent Android lifecycle
steps. A loader may put its existing fake-EGL or engine adapter above it.

## V4 (0.3.5): `is_fatal` sem trava

A 0.3.5 torna `nxgl_frame_proof_is_fatal()` um load atômico: nunca devolve
"fatal" só porque a thread de render está dentro de uma leitura legítima.
Ver CHANGELOG 0.3.5 e `tests/test_frame_proof_fatal_race.c`.

## V4 (0.3.4): fatal de vídeo fecha o frame loop Godot

O frame-proof expõe estado fatal irreversível e um pedido de fechamento
one-shot. `nxgl_godot_frame_proof_before_swap()` continua retornando `int`, mas
agora devolve `NXGL_GODOT_FRAME_PROOF_FATAL` (`-2`) assim que a terceira
amostra conclusiva vira BLACK/DEAD-CONTEXT e em toda tentativa posterior. O
DisplayServer só pode executar o present quando o retorno for 0; ao consumir o
close, encerra o loop e usa `nxgl_godot_frame_proof_exit_status()` (72), nunca
status limpo. Health fica bloqueado permanentemente e o receipt seguro capturado
no launch é revogado no primeiro consumo fatal. OK não pede close nem muda o
status. O marcador de integração passa a `nxgl-godot-frame-proof/2`.

## V4 (0.3.3): encaixe Godot versionado

`engine-glue/nxgl_godot_frame_proof.h` fixa, sem depender da ABI C++ da
engine, os cinco hooks necessários para Godot 3/4: recibo antecipado, resolver
do contexto corrente, metadados do drawable, amostra imediatamente antes do
swap real e publicação antes de destruir GL. Patches e resolvers continuam
separados por major/versão; a camada não cria janela nem present.

## V4 (0.3.2): recibo fatal de vídeo

O adapter opt-in de frame proof pode publicar no caminho privado
`NXBOOTSTRAP_VIDEO_FILE` um recibo JSON vinculado ao mesmo `run_id`, geração e
port do health receipt. `OK` só nasce no framebuffer padrão, imediatamente
antes do present, de pixels RGB não pretos cujo alpha também é diferente de
zero. Assim, RGB colorido com alpha zero continua `BLACK`, como exige o
compositor OSD Amlogic. Três amostras consecutivas before-present — nunca uma
amostra final isolada — publicam `BLACK`/`DEAD-CONTEXT`; o fatal é irreversível
mesmo se o processo e o áudio continuarem vivos. Preto observado em lançamento
remoto/desconhecido segue inconclusivo e não acusa o port. Um
`NXLAUNCH_FRONTEND=1` explícito vence uma sessão SSH paralela; `=0` não é prova
de frontend.

O limite de alpha é propositalmente literal: `0` não é visível e `1..255` é
não-zero. Isso fecha o defeito comprovado de alpha exatamente zero; não afirma
um limiar perceptual de brilho do painel para valores baixos, que exigiria uma
medição física diferente.

O preflight observa sem alterar: draw/read FBO precisam ser zero, PBO de pack
precisa estar desligado e row/skip de pack precisam ser compatíveis. A leitura
usa memória inicializada, guard e dois sentinels; escrita ausente/parcial nunca
é classificada. O adapter jamais chama `glGetError`, portanto não drena a fila
do jogo. Dimensões, stride e toda aritmética ficam limitados a 64 MiB.

O arquivo é `0600`, regular, owner-only, com um link, temporário exclusivo e
rename atômico dentro do runtime privado. Path e tuple são copiados na primeira
publicação e não podem ser desviados por mudança posterior do ambiente. Se a
troca fatal falhar após um OK, o OK anterior é revogado, o fatal fica pendente
e a próxima chamada tenta de novo; temporário exclusivo que o adapter não
criou nunca é apagado. Quando
`NXLAUNCH_PROOF_DIR` pede PNG, somente a exata primeira amostra que poderia
gerar OK vira RGBA PNG, também por escrita atômica; falha do PNG impede o OK e
fica explícita no log. O adapter nunca cria nem valida
`NXBOOTSTRAP_HEALTH_FILE`: prova de vídeo e saúde da geração são autoridades
separadas. Na 0.3.4, o primeiro consumo fatal remove somente um receipt regular,
0600, owner-only e sem hardlink no path capturado no launch; o status não zero
continua sendo a autoridade mesmo se a revogação for impossível. As entradas
stateful exigem a thread de render. O
owner mantém o guard durante a medição, mas a entrada faz um único test-and-set
sem espera ou spin; concorrência/reentrada por callback é recusada de imediato
e não pode esperar por si mesma.

## V4 (0.3.1): fronteira post-first-present

`nxgl_graphics_present_gate.h` — **V4-GRAPHICS-04**, opt-in declarativo
(`graphics.evidence_boundary=post-first-present`). Em backends Wayland o
drawable de uma janela/contexto válidos fica em `1x1` até o guest apresentar o
primeiro buffer; a API one-shot, que espera o resize antes de devolver o
controle, forma um ciclo (o guest não recebe o controle, logo nunca apresenta).
O gate divide a prova em duas fases: um **preflight** pré-present que valida
contrato, API/profile/versão, provider e shader e representa `1x1` como
PENDENTE (nunca sucesso, nunca receipt), e uma observação **pós-present**,
chamada pelo wrapper somente depois do `SDL_GL_SwapWindow`/`eglSwapBuffers`
real do guest, que promove drawable útil a `PROVED` uma única vez, com deadline
monotônico contado a partir do primeiro present. `1x1` persistente continua
falhando fechado. O nxgl não limpa, não desenha e não faz swap sintético; nada
é decidido por GPU, CFW, device ou nome de jogo, e sem o opt-in a API 0.3.0
permanece literal.

## V4 (0.3.0): apresentação e binding EGL

Duas superfícies novas, ambas **opt-in por port** e desligadas por omissão.

`nxgl_display.h` — política pura de apresentação. Recebe a política declarada,
o aspecto interno e o drawable **medido** e devolve o content rect mais as duas
transformadas. **Ausência de política é `game`: o framework não instala nada** e
o comportamento de todo port já aprovado fica idêntico. `preserve` (letterbox)
muda pixels e por isso é opt-in explícito, nunca default. Toque na barra é
descartado, nunca grudado na borda. Nada é decidido por GPU, CFW, device ou
nome de jogo.

`nxgl_egl_binding.h` — liga os imports EGL de um guest Android relocado ao
provider que realmente possui o contexto corrente. O candidato abre
`RTLD_LOCAL`, prova todos os imports declarados, prova contexto não nulo e
prova que é o mesmo objeto que o resolver do SDL usou; só então é promovido a
`RTLD_GLOBAL`. Nada é globalizado sem prova, não há segundo contexto/surface e
o executável universal continua sem `DT_NEEDED libEGL`.
`tools/nxgl-egl-audit.py` lê o ELF real e confere inventário, `DT_NEEDED` e o
teto de GLIBC.

## Explicit RED coverage compatibility

Version 0.2.17 adds the opt-in
`NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT` for the proven Unity/TextMeshPro
single-channel split. A fully measured native ES3 route remains
`GL_R8`/`GL_RED` and uses the existing alpha-mask swizzle. Only a measured
legacy route with fallback explicitly allowed becomes `GL_LUMINANCE_ALPHA`;
each source coverage byte is then emitted as `(R,R)`, including TexImage and
TexSubImage data with tracked unpack alignment, row length and skips. The exact
contract fixture is `00 7f ff -> 00 00 7f 7f ff ff`.

The semantic is additive and never selected by GPU, device, firmware or engine
name. `PRESERVE=1` and `ALPHA_MASK=2` retain their original numeric values and
byte behavior; in particular ALPHA_MASK fallback is still `(255,R)`. The old
context-free API and `duplicate_r_to_la()` remain quarantined. Ports that
already own a contiguous CPU coverage buffer may use the bounded
`nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous()` helper,
but GL uploads must use the v2 planner/converter so pixel-store layout and PBO
state remain fail-closed.

The adapter only observes capabilities and returns plans. Resolving
`glTexStorage2D` establishes availability; neither measurement nor any plan
function invokes, wraps or replaces TexStorage. The caller continues to own
the original GL call and every state mutation.

## Target-ABI SDL video-hint sanitizer

Version 0.2.16 adds a separately linked, default-off nxloader 0.7.2 provider
for the complete graphics-evidence barrier. A port installs it after its normal
providers and before its first `nxloader_module_resolve()`. It captures the
selected originals and overrides only `MakeCurrent`, `GetProcAddress`, and
`glCreateShader`: successful MakeCurrent measures the live context/drawable,
compiles and links the contract-specific shader, writes the versioned JSON
receipt, and only then releases shader creation. A bad context, 1x1 drawable,
shader failure, or rejected receipt fails closed. The base library and every
existing port remain unchanged unless both the build option and runtime flag
are selected.

Provider API 2 requires an explicit release binding from the port adapter:
framework commit, CFW, device, port version and the SHA-256 of the exact game
ELF selected for the run. Only run id, generation id and port id may fall back
to nxbootstrap 0.6.32's run-bound `NXBOOTSTRAP_HEALTH_*` exports; explicit and
exported values must agree. Ambient `NX_DEVICE`, `NX_ARTIFACT_SHA256` and the
other historical `NX_*` names have no authority in this provider. The runtime
generation id and executable hash remain two separate identities.

The 0.2.16 single-channel API is likewise still opt-in, but its old unscoped
adapter path is quarantined. The hardened path requires an explicit semantic
contract, keys binding state by context/thread/texture-unit/target, keys texture
objects by share group/target/id, tracks pixel-store and PBO state, checks
subrect/type/size arithmetic, and preserves RED, ALPHA and LUMINANCE semantics
separately. Fallback CPU expansion rejects PBOs and unsupported types; RGBA and
compressed/unknown formats pass through untouched.

Version 0.2.15 adds the V3 graphics contracts: adapter-declared EGLConfig
requirements with an appended EGLCONFIG: receipt (all-don't-care default —
one game's RGBA8888 never becomes global), the pure single-clean-retry state
machine, the ordered candidate trace in the GLES1 receipt and the frame-proof
sampling-point field, all byte-compatible with the existing receipt lines.

Version 0.2.14 replaces the GLES1 name-ordered provider chain with
per-candidate SET resolution proven live: with the context current, the
candidate's own `glGetString(GL_RENDERER)` must answer before the set is
accepted. Measured on the official ROCKNIX RK3566 image, `libmali.so` is an
orphan kbase blob while the versioned Mesa is the live dispatch -- the exact
mirror of dArkOS -- so no fixed name order can be right on both firmwares. A
port may inject `SDL_GL_GetProcAddress` as the primary resolver
(`nxgl_gles1_set_primary_resolver`), tried before any dlopen. With no live
candidate the v1 first-complete-set behaviour is kept and the receipt reports
`liveness=dead` instead of posing as healthy.

Version 0.2.13 lets the reactive repair override an inherited provider hint
that live measurement has just disproven: when the context created under the
hint reports a NULL renderer, the hint stops outranking the repair, the
override is recorded in the receipt and the hinted values return on a failed
exec. The pre-context path still treats the CFW hint as sovereign. This is
the dArkOS menu-launch field case (frontend units export
`SDL_VIDEO_EGL_DRIVER`), which SSH launches could never reproduce.

Version 0.2.12 replaces 0.2.11's pre-re-exec EGL probe (it refused a good
gbm-platform blob, whose display only exists through GBM) with the real
attempt as the live criterion: the re-executed process may unbind the applied
provider pair once, via `nxgl_provider_precontext_rollback()`, when window
creation fails again — the port then retries through the firmware's normal
stack and the final error is the true one. Without the repair marker the
rollback never touches the environment.

Version 0.2.10 adds a runtime OpenGL ES 1.1 resolver (`nxgl_gles1`) for ports
whose engine is fixed-function, so they need no `DT_NEEDED libGLESv1_CM` and
never bind a crossed versioned SONAME that resolves to a driverless Mesa.

Version 0.2.9 adds a default-off startup helper for a mixed-ABI launcher that
can inherit `SDL_VIDEODRIVER` from a frontend using a different SDL build. The
adapter calls it inside the game process, after that process has loaded its
target SDL and before any `SDL_INIT_VIDEO` call. It queries
`SDL_GetNumVideoDrivers()` and `SDL_GetVideoDriver()` from that exact SDL.

The policy is deliberately capability-based:

- no inherited hint leaves SDL autodetection unchanged;
- a hint naming a driver compiled into the target SDL is preserved exactly;
- an inherited hint unsupported by the target SDL is removed, with no
  replacement selected;
- malformed input, an unavailable driver inventory, a running video subsystem
  or arbiter contention fails closed without guessing a backend.

The helper changes only the canonical `SDL_VIDEODRIVER` variable on the one
unsupported-hint path. It never changes `SDL_VIDEO_DRIVER`,
`SDL_DYNAMIC_API`, `SDL_VIDEO_EGL_DRIVER`, `SDL_VIDEO_GL_DRIVER`,
`LD_PRELOAD` or any EGL/GLES provider name. It has no firmware/device table
and does not force KMSDRM, fbdev, Wayland or another backend.

```c
nxgl_sdl_video_hint_options_v2 options;
nxgl_sdl_video_hint_receipt_v2 receipt;
char line[NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX];

nxgl_sdl_video_hint_options_v2_init(&options);
options.enabled = 1; /* explicit adapter opt-in */
if (nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) < 0)
  return startup_failure();

if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
  return startup_failure();
if (nxgl_complete_sdl_video_hint_receipt_v2(&receipt) != NXGL_SUCCESS)
  return startup_failure();
if (nxgl_format_sdl_video_hint_receipt_v2(&receipt, line,
                                           sizeof(line)) == NXGL_SUCCESS)
  write_port_log(line);
```

Environment access must occur during the adapter's serialized startup, before
it creates worker threads. Initializers keep the feature disabled, so existing
ports and the separate AArch64 NXExtract process retain their prior behavior.
NXSplash is likewise outside this API and remains an independent handoff.

## Provider recovery prefilter

Adapters that have measured a broken resolved EGL/GLES provider may use
`nxgl_provider_name_compatible()` before their own symbol and live-EGL probes.
The helper is deliberately passive: it never scans a directory, loads a
library, changes `LD_PRELOAD`, or re-executes a process.

Provider names are classified by the transport they explicitly advertise.
For a direct KMSDRM path, a pure `-wayland` or `-x11` object is rejected, but a
hybrid name such as `-wayland-gbm` remains eligible because it explicitly
advertises GBM. `dummy`, `stub`, `headless`, and `surfaceless` objects are
always rejected. Passing this prefilter is not proof of compatibility: the
adapter must still resolve every required entry point, initialize EGL against
the live kernel, and validate the real window/context stack before use.

Since 0.2.3, `nxgl_plan_sdl_provider_pair()` records the stricter recovery
contract for crossed SDL provider paths. It authorizes an adapter-owned
one-shot re-exec only after a real window, current context and positive
drawable exist while `GL_RENDERER` is empty, and only when one transport-safe
candidate exports both EGL and the engine's GLES entry points. A positive
plan means the adapter binds both `SDL_VIDEO_EGL_DRIVER` and
`SDL_VIDEO_GL_DRIVER` to that exact object. The helper never changes the
environment, scans libraries or re-executes, and a healthy renderer always
returns no action.

Version 0.2.4 adds a separate pre-context authorization helper for the case
where SDL starts a real direct-display backend but exhausts its complete
candidate ladder at context creation because the firmware resolves crossed
EGL/GLES providers. `nxgl_plan_sdl_precontext_recovery_v2()` is fail-closed:
it requires the exact context-create stage/reason, an exhausted first pass, no
inherited provider override, one identical object for EGL and GLES, both
required symbol sets, and a transport-compatible provider name. It remains a
pure plan. The adapter owns discovery, `dlopen`/symbol proof, environment
snapshot and restore, and exactly one retry. A failed v2 report retains the
sanitized backend that actually started so this decision need not infer a
firmware or device name. Version 0.2.5 recognizes both exact load boundaries
used by SDL implementations: exhausted `WINDOW_CREATE/WINDOW_FAILED` or
`CONTEXT_CREATE/CONTEXT_FAILED`. Mixed stage/reason pairs remain rejected.

Version 0.2.6 adds the optional `nxgl-provider-recovery` archive for the
effectful part that had been repeated by proven adapters. It is deliberately a
separate archive: linking ordinary `nxgl` still brings in no provider scan,
`dlopen`, environment mutation or re-exec behavior. Its option initializers set
`enabled=0`, so even linking the helper cannot activate recovery.

The common sequence is intentionally narrow:

1. the adapter records the real nxgl failure and explicitly selects one
   candidate path or SONAME; the helper never enumerates firmware directories;
2. if the adapter requests the live `EGL_DEFAULT_DISPLAY` probe, it first
   completes native video teardown and sets the explicit `video_torn_down`
   attestation; `SYMBOLS_ONLY` makes no EGL lifecycle call;
3. `nxgl_probe_sdl_provider_v2()` proves that EGL and every engine-specific
   GLES symbol declared by the adapter come from the same canonical regular
   DSO; pre-context recovery may additionally require a live
   `eglInitialize(EGL_DEFAULT_DISPLAY)` followed by `eglTerminate`;
4. the adapter feeds that receipt into the existing post-output or pre-context
   pure plan and ensures native video teardown is complete;
5. only a positive plan may reach `nxgl_reexec_sdl_provider_pair_v2()`, which
   rechecks the file identity, refuses inherited provider overrides and a
   previous one-shot marker, binds both SDL provider variables to the same
   absolute object and re-executes `/proc/self/exe` once.

If re-exec fails, the helper removes every variable it introduced before
returning the real error. It never writes `LD_PRELOAD`; a port whose private
guest loader needs another provider route keeps that engine-specific route in
its adapter. Candidate discovery, required GLES symbols and lifecycle teardown
also remain adapter-owned and opt-in. Existing approved ports are not migrated
or regenerated by this release.

The receipt's device/inode/size/mtime recheck detects accidental operational
replacement; it is not a security boundary against a malicious writer or an
uncoordinated same-process thread. From probe through re-exec, the adapter must
keep the candidate in a local immutable tree (at minimum, not writable by an
untrusted actor) and coordinate every same-process writer. There remains an
unavoidable TOCTOU window before SDL opens the path after re-exec.

If the live probe initializes a display but `eglTerminate` fails, the receipt
is unusable and the helper deliberately retains its `dlopen` handle: unloading
code that may still own a live display would be unsafe. The adapter must abort
startup. The helper also poisons itself for the rest of the process: every
later otherwise-valid enabled probe and every otherwise-valid authorized
re-exec returns `EGL_TERMINATE_FAILED` without touching the provider, display
or environment, including attempts using an older valid receipt.

## Evidence used

The sanitized source of truth is
`references/m13-video-evidence-v1.json`. It separates approved working ports
from historical facts, narrow quirk provenance and future pilots:

| Reference | Classification | Rule retained by nxgl |
|---|---|---|
| Horizon Chase | approved positive, multi-stack | Select ownership from the backend actually opened; keep KMSDRM/Wayland present inside SDL ownership; use real GL strings and drawable. |
| Castle of Illusion | approved positive, multi-device | Keep global NPOT wrap rewriting off; inspect sampler/wrap/atlas first for a geometrically correct black silhouette; use the real drawable. |
| Sonic 4 EP2 AArch64 | approved positive, release-scoped | Request an ES profile, reject measured desktop GL, recreate window/context per config candidate, and wait for a positive drawable. |
| Bully2 | historical/delisted narrow fact only | Cross-check same-stack EGL symbol resolution; it is not a current positive whole-port reference and contributes no copied implementation. |
| LEGO Star Wars TFA | alpha-one quirk provenance only | Name the Amlogic default-backbuffer alpha-zero symptom; the workaround remains exact, adapter-owned and opt-in. |
| Chrono Trigger | negative/designed-only pilot | Supplies no positive runtime or physical evidence to M13. It becomes evidence only after a future framework migration and authorized device gate. |

The physical rows above are imported, hash-pinned release history; **M13 did
not run a device or guest**. Port-specific fixes did **not** become global
behavior. In particular, nxgl never enables an FBO clear, forces texture wrap,
alters a shader, changes an engine render scale, or skips lifecycle work.

## Runtime contract

The normal startup order is:

```text
PortMaster launcher
  -> nxcompat preflight/environment plan
  -> SDL video negotiation
  -> nxgl real-output negotiation
       window -> GLES context -> delivered config -> real drawable
  -> engine/Android adapter lifecycle
```

Call the negotiation before the engine creates any SDL window or render
thread. The inherited-hint recovery restarts the SDL video subsystem; it is
not a hot-recovery API for a running game.

The API-v1 entry points remain source/ABI compatible. New integrations use
API v2 (`NXGL_API_CURRENT_VERSION`): the caller declares one stack owner and
nxgl freezes that provider's callbacks, handles and userdata through
`nxgl_close_v2()`. Provider callback code and userdata must outlive every
open/present/rollback/close callback. Report handle copies are observations,
not ownership transfers, and become stale at close.

`nxgl_open()`/`nxgl_open_v2()` can initialize `SDL_INIT_VIDEO` if necessary,
but they never assign a replacement backend name to
`SDL_VIDEODRIVER`/`SDL_VIDEO_DRIVER`,
sets `MESA_GLES_VERSION_OVERRIDE`, selects a card number, or fixes a
resolution. If nxcompat already initialized SDL, nxgl still
performs the real window/context/drawable gate; `SDL_InitSubSystem()` alone is
not treated as proof that video works.

All mutating API-v1 and API-v2 entry points share a non-blocking nxgl arbiter.
Open, make-current and present return byte-atomic `NXGL_ERROR_BUSY` before
touching outputs or graphics state; the legacy void close is a no-op while
busy. API-v1 accessors reject API-v2 contexts so they cannot bypass the frozen
stack. The adapter must still serialize the preceding nxcompat transaction
with nxgl startup; nxgl does not reach into nxcompat's private arbiter.

Resolution precedence is fixed and capability-based:

1. `SDL_GetDesktopDisplayMode()`;
2. `SDL_GetCurrentDisplayMode()`;
3. `SDL_GetDisplayBounds()`;
4. DRM dimensions passed by the preflight probe;
5. fbdev dimensions passed by the preflight probe.

There is no built-in `1280x720` fallback. After context creation,
`SDL_GL_GetDrawableSize()` is authoritative and must become positive. The
default 300 ms window pumps asynchronous compositor configure events; if the
drawable differs from the requested window, the last real value wins.

## Declaring an engine ladder

The caller declares what the engine actually supports. This GLES2 example
keeps RGBA8888 mandatory and relaxes only depth/stencil:

```c
#include "nxgl.h"

static const nxgl_config_candidate configs[] = {
    {2, 0, 8, 8, 8, 8, 24, 8, 1},
    {2, 0, 8, 8, 8, 8, 16, 0, 1},
    {2, 0, 8, 8, 8, 8,  0, 0, 1},
};

nxgl_engine_requirements requirements;
nxgl_open_options options;
nxgl_context *graphics = NULL;
nxgl_report report;

nxgl_engine_requirements_init(&requirements);
requirements.minimum_alpha_bits = 8;
requirements.minimum_depth_bits = 0;

nxgl_open_options_init(&options);
options.window_title = "My Port";
options.requirements = &requirements;
options.candidates = configs;
options.candidate_count = sizeof(configs) / sizeof(configs[0]);

if (nxgl_open(&options, &graphics, &report) != NXGL_SUCCESS) {
    /* Show the error through the port's normal startup/error path. */
}
```

The default request floor is GLES2 with RGB888 and double buffering. GLES3 is
never inserted into the request ladder automatically. A real GLES3 context
returned by the driver for an ES2 request is accepted because it still exposes
the GLES2 API; an engine that requires an exact maximum can declare one. If an
engine is proven to need an explicit GLES3 retry, its adapter adds that
candidate after the GLES2 candidates. Nothing in nxgl asks Mesa for “3.2”.

Every candidate sets all RGB, alpha, depth, stencil, double-buffer, profile,
version, and no-MSAA attributes before creating its own window and context.
API v2 then requires a current same-stack EGL display/context/surface, finds
the delivered `EGLConfig` by its real config ID, checks ES2/window capability,
and compares the positive EGL surface size with the drawable. The delivered
values must satisfy the engine requirements.

## Backend retry and desktop-GL rejection

The first full attempt uses the environment exactly as inherited. Each failed
candidate releases its own window/context before the next candidate. Only
after the complete inherited path fails, and only when nxgl initialized and
therefore owns SDL video, may the default policy:

1. remove an inherited `SDL_VIDEODRIVER`/`SDL_VIDEO_DRIVER`;
2. restart SDL video without naming another backend;
3. repeat the real-output gate once.

If SDL returns a desktop context, nxgl destroys it. The default policy may
retry that same candidate once with `SDL_HINT_OPENGL_ES_DRIVER=1`; on an
already selected X11 backend it also requires the EGL rather than GLX hint.
Both the environment and SDL hints are snapshotted dynamically, restored
exactly (absent, empty or full value), and verified before success is
published. There is no third attempt. This chooses the GLES API required by
the engine; it does not choose a firmware backend or GL version. The bounded
attempt journal and status callback record fallback actions and stable reasons
without retaining provider error text, paths, IPs or tokens.

Since 0.2.2, `SDL_ResetHint` is an optional runtime symbol rather than a link
floor. On SDL 2.0.4 through 2.23, if a hint was absent and therefore cannot be
restored exactly, nxgl disables only the optional GLES/EGL hint-recovery retry.
The ordinary candidate round still runs, and nxgl does not approximate absence
with an empty value or clear unrelated process hints.

If SDL video was already caller-owned, nxgl never quits or restarts it for
autodetection; failure is returned with the inherited environment intact.

## Status/logo callback and nxcompat

The API-v1 `nxgl_status_callback` receives bounded lines for resolution, each
attempted config, fallback actions, the selected profile, and errors. API v2
keeps per-attempt detail in its bounded, finite-reason journal and sends only
the terminal selected/error notification through the callback. A port may
route that notification to stderr and to text over its existing startup logo;
nxgl does not create a competing splash screen. Callback code and userdata are
borrowed by a successful context and must remain alive until its final close;
callbacks run under the nxgl arbiter and must return promptly.

For API v2, the terminal selected/error notification runs inside the
transaction and arbiter, before either output is published. It must return
promptly. An nxgl open/close/present call re-entered from that notification
returns byte-atomic BUSY; after the callback returns, nxgl publishes the
context/report (or cleanup handle) and releases the arbiter.

With `-DNXGL_WITH_NXCOMPAT=ON`, `libnxgl-nxcompat.a` adds two adapters:

- `nxgl_nxcompat_resolution_sources()` passes the already-probed DRM/fbdev
  dimensions to nxgl;
- `nxgl_nxcompat_capture_report()` converts a free-form `nxgl_report` into the
  legacy diagnostic profile and emits the shared graphics status line. This
  call is diagnostic-only and **never** satisfies a capability requirement;
- `nxgl_nxcompat_publish_context()` accepts an opaque, already-open
  `nxgl_context`, verifies that its SDL window/context are current, requeries
  GL strings, the delivered SDL config and positive drawable, then resolves
  the current EGL display/context/config through `SDL_GL_GetProcAddress()` and
  transactionally publishes the typed receipt to `nxcompat_registry`.

Since 0.2.4 the strong bridge supports both API-v1 contexts and API-v2
SDL/EGL contexts. For API v2 it consumes only the frozen handles in the v2
report and verifies they are already the exact current SDL window/context; it
does not route through the intentionally rejected API-v1 accessors or call a
second make-current provider. Raw-EGL v2 ownership remains adapter-owned and
is rejected by this SDL-specific bridge.

The strong bridge does not call `nxgl_open()`, create or destroy resources,
swap buffers, alter GL state, or select a backend. A missing EGL symbol creates
an honest partial receipt: window/GLES/drawable evidence may remain valid while
`graphics.egl` and `graphics.egl-config` stay absent/lost. Required-capability
evaluation, rather than renderer-name inference, decides whether that is fatal
for the port. Receipt generations must increase for a given registry; stale or
malformed publications leave the previous state byte-identical.

The adapter links to nxcompat without changing nxcompat itself.

## Presentation ownership

The default `nxgl_present_policy` is `NXGL_PRESENT_ENGINE_OWNED`, a strict
no-op. Linking the library cannot add a second swap or alter the framebuffer.
An engine adapter must explicitly select one of:

- `NXGL_PRESENT_SDL` for `SDL_GL_SwapWindow()`;
- `NXGL_PRESENT_ADAPTER` with a callback for raw EGL, a firmware blitter, or
  another proven path.

Two optional flags are also adapter decisions:

- `NXGL_PRESENT_FINISH_BEFORE_SWAP` calls `glFinish()`;
- `NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE` performs an A-only clear while
  preserving scissor, color mask, and clear color; it refuses to run while an
  off-screen FBO is bound.

Neither flag is inferred from a device name, GPU name, or SDL backend. API v2
accepts alpha-one only with the exact named Amlogic quirk and an explicit
observed-alpha-zero reason, on FBO 0 with a real alpha channel. It snapshots
and verifies the framebuffer binding, and restores plus verifies scissor
enablement, all four color-mask bits and clear color before the provider
performs one present. `glFinish`
remains a separate explicit flag and is never global. A raw-EGL provider must
resolve and validate the same stack that created its context; SDL ownership
uses the SDL provider's present callback.

## Surface lifecycle and dimensions

API v2 exposes passive observations for focus, minimize/restore, resize and
context loss/recreation. These calls update a caller-owned state machine and
monotonic generations; they do not recreate a native surface, invent Android
lifecycle events or call the engine. Malformed booleans, partial dimensions,
out-of-range values and generation overflow fail without changing the state.

`nxgl_calculate_surface_metrics_v2()` keeps display, drawable, viewport and
render-target dimensions distinct and reports their measured ratios. It never
assumes that requested window size equals scanout, drawable or internal render
scale. The pure silhouette classifier only prioritizes a sampler/wrap/atlas
audit when black pixels preserve a correct geometric silhouette; it never
clears an FBO, swaps a shader or converts a texture.

## Texture policy

nxgl does not hook texture calls. NPOT workarounds and forced
`CLAMP_TO_EDGE` are therefore **off by construction**. If one game proves that
it needs a sampler workaround, that belongs in its engine adapter and must not
become a global default. This preserves repeated/mirrored atlas UVs on
Mali-450 and other GLES2 drivers.

## Build and tests

```sh
cmake -S framework/nxgl -B /tmp/nxgl-build \
  -DNXGL_BUILD_TESTS=ON \
  -DNXGL_BUILD_NATIVE_TESTS=OFF \
  -DNXGL_WITH_NXCOMPAT=ON \
  -DNXGL_ENABLE_SANITIZERS=ON
cmake --build /tmp/nxgl-build --parallel
ctest --test-dir /tmp/nxgl-build --output-on-failure
```

Adapters opting into the provider transaction include
`nxgl_provider_recovery.h` and link `nxgl::provider-recovery` in addition to
`nxgl::nxgl`. The normal core needs neither.

This command is the hermetic M12 receipt gate. With
`NXGL_BUILD_NATIVE_TESTS=OFF`, it builds
only the nxcompat bridge test, whose SDL/GL/EGL symbols and opaque context are
entirely test-owned. It never calls `nxgl_open()` and does not require a host
GPU, window system, EGL driver, device, or session mutation.

M13 has a separate canonical runner:

```sh
bash framework/nxgl/tests/run-m13-host.sh
```

It compiles the API-v2 open, present, lifecycle, metrics, SDL-hint and
diagnostic fixtures with GCC and Clang sanitizers. Forbidden-symbol and
dynamic-dependency barriers run before the fixtures, so the automatic gate
cannot initialize SDL video, create a window/context, call EGL/GLES or touch a
GPU/session/device. Its result deliberately records
`physical_device_evidence=0`; imported release history is not a new hardware
run.

`NXGL_BUILD_NATIVE_TESTS=ON` enables the separate native-linked policy suite
for intentional/manual host validation. It is not part of the hermetic M12
receipt gate and must not be confused with physical device proof. Both paths
compile as C99 with strict warnings; the M12 bridge path is the safe default.

The output is a static library intended to be compiled into each loader.
Public packages still need to audit the **final loader and every bundled Linux
ELF** for the project-wide `GLIBC <= 2.30` gate; a host test build is not a
release artifact.
