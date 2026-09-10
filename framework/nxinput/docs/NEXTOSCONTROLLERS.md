# NEXTOSCONTROLLERS.gptk — format contract (v1 e v2)

**PT-BR:** `NEXTOSCONTROLLERS.gptk` é o formato PRÓPRIO do NextOS para mapear
controles físicos simbólicos (A, B, L2, RIGHT_STICK, …) em AÇÕES semânticas
(`ui.confirm`, `player.jump`, …) por contexto (`menu`, `gameplay`, `cursor`).
**Não é gptokeyb e nunca converte controle em tecla de teclado.** Códigos
evdev numéricos são rejeitados de propósito (regressão do Chrono Trigger, em
que códigos crus transformaram L2/R2 em START/SELECT em outro pad). A cópia
editada pelo usuário é estado do dono: nunca é sobrescrita; defaults novos
chegam como `.new` ao lado.

**EN:** `NEXTOSCONTROLLERS.gptk` is the NextOS-own format that binds symbolic
physical controls to semantic actions per context. **It is NOT gptokeyb and
never converts controller input into keyboard keys.** Numeric evdev codes are
rejected by design. The user's edited copy is owner state: it is never
overwritten; new defaults land next to it as `.new`.

- APIs: `include/nxinput_gptk.h`, `include/nxinput_gptk_loader.h`,
  `include/nxinput_exit_chord.h`
- Implementations: `src/nxinput_gptk.c`, `src/nxinput_gptk_loader.c`,
  `src/nxinput_exit_chord.c`
- Tests: `tests/test_gptk*.c`, `tests/test_exit_chord.c`, runner
  `tests/run-gptk-host.sh`

## Flow

```
controle físico (botão/eixo do device)
        │
        ▼
mapping SDL / firmware (SDL_GAMECONTROLLERCONFIG, PortMaster)
        │
        ▼
nxinput (estado lógico por pad: botões, sticks, gatilhos)
        │
        ▼
NEXTOSCONTROLLERS.gptk (contexto ativo: menu / gameplay / cursor)
        │
        ▼
ação semântica (ui.confirm, player.jump, cursor.move, …)
        │
        ▼
sinks registrados (Android/so-loader, SDL, touch-injection, …)
        │
        ▼
jogo
```

The dispatcher (`nxinput_gptk_dispatcher`) sits between the gptk map and the
sinks: it is edge-triggered, latches each pressed control, fans one logical
press out to EVERY sink registered for that action exactly once, and releases
every latched control into the OLD context before a context switch completes.

## Grammar

The file is plain UTF-8 text, line oriented. `#` starts a comment (whole
line). Blank lines are ignored. Spaces and tabs around tokens are ignored;
`\r\n` line endings are accepted.

```
file      := magic port? section+
magic     := "format = NEXTOS_CONTROLLERS/1"        ; first non-comment line,
                                                    ; byte for byte
port      := "port" "=" port_id                     ; optional, before any
                                                    ; section, at most once
port_id   := [a-z0-9._-]{1,64}
section   := "[" name "]" entry*                    ; name in: menu,
                                                    ; gameplay, cursor, camera
entry     := mapping                                ; menu, gameplay, cursor
           | cursor_tuning                          ; cursor only
           | camera_tuning                          ; camera only
mapping   := CONTROL "=" action
CONTROL   := A | B | X | Y | L1 | R1 | L2 | R2 | L3 | R3 | START | SELECT
           | UP | DOWN | LEFT | RIGHT | LEFT_STICK | RIGHT_STICK
action    := [a-z][a-z0-9_]*("."[a-z][a-z0-9_]*)+   ; max 64 chars total

cursor_tuning := ("speed" | "deadzone" | "response_curve"
               | "acceleration" | "smoothing_ms") "=" number
camera_tuning := ("sensitivity_x" | "sensitivity_y" | "deadzone"
               | "response_curve") "=" number
               | ("invert_x" | "invert_y") "=" ("true" | "false")
               | "authority" "=" ("nextos" | "native")
number    := "-"? digit+ ("." digit+)?              ; strict local decimal:
                                                    ; no hex, no exponent,
                                                    ; no nan/inf, no locale
```

Rules:

- `[menu]` and `[gameplay]` are REQUIRED for the file to be accepted;
  `[cursor]` and `[camera]` are optional.
- `[cursor]` may mix control->action lines with its tuning keys. `[camera]`
  is a TUNING-ONLY section: any control or non-camera key there is NXI1001.
- Tuning keys are valid only in their own section: `speed = 1.0` inside
  `[menu]` or `[gameplay]` is NXI1001, like any other unknown key.
- Control names are the symbolic vocabulary above and nothing else. A token
  made of digits (an evdev code such as `304`) is always rejected.
- The same control may not appear twice inside one section.
- Actions carry at least one `.` (namespace), lowercase only. Examples:
  `ui.confirm`, `player.jump`, `cursor.move`, `firstboot.accept`.
- `L2`, `R2`, `LEFT_STICK`, `RIGHT_STICK` are analog-capable: the dispatch
  value carries the magnitude 0..1. Digital controls use 0/1.
- No shell expansion, no includes, no evaluation of any kind. The parser
  reads only the memory buffer it is given; the caller performs the
  symlink-safe file read.

### Example

```
# NEXTOSCONTROLLERS example
format = NEXTOS_CONTROLLERS/1
port = examplegame

[menu]
A = ui.confirm
B = ui.cancel
RIGHT_STICK = cursor.move
R3 = cursor.click

[gameplay]
A = player.jump
B = player.action
RIGHT_STICK = camera.move
```

## Tuning (V3): `[cursor]` / `[camera]` numeric keys

Universal cursor and camera feel, per port, in the same file. All keys are
**opt-in**: a file without tuning keys behaves byte-identically to before —
the defaults below are applied, and the defaults reproduce the behavior of
the already-approved ports. Never ship a tuning block just to restate the
defaults.

```
[cursor]
speed = 1.00          # screen-heights per second at full deflection
deadzone = 0.15
response_curve = 1.60
acceleration = 0.35
smoothing_ms = 70

[camera]
sensitivity_x = 1.00
sensitivity_y = 1.00
deadzone = 0.15
response_curve = 1.00
invert_x = false
invert_y = false
authority = nextos    # or: native
```

### Keys, units and bounds

| Section | Key | Unit / meaning | Bounds | Default |
|---|---|---|---|---|
| `[cursor]` | `speed` | screen-**heights** per second at full deflection (resolution-independent: scaled by the drawable height) | 0.05 .. 8 | 1.0 |
| `[cursor]` | `deadzone` | fraction of full deflection, **radial** (vector magnitude, not per-axis) | 0 .. 0.9 | 0.15 |
| `[cursor]` | `response_curve` | `pow()` exponent applied to the normalized magnitude (1 = linear, >1 = finer near center) | 0.25 .. 4 | 1.0 |
| `[cursor]` | `acceleration` | extra gain that grows with deflection: full-deflection speed is `speed * (1 + acceleration)`; magnitude-based, never time-based | 0 .. 4 | 0.0 |
| `[cursor]` | `smoothing_ms` | time constant (ms) of the exponential velocity smoothing; 0 = raw | 0 .. 500 | 0.0 |
| `[camera]` | `sensitivity_x` / `sensitivity_y` | unitless per-axis multiplier on the shaped output | 0.05 .. 8 | 1.0 |
| `[camera]` | `deadzone` | radial, with **rescaling**: full deflection still reaches magnitude 1 (no resolution loss above the deadzone) | 0 .. 0.9 | 0.15 |
| `[camera]` | `response_curve` | `pow()` exponent on the normalized magnitude | 0.25 .. 4 | 1.0 |
| `[camera]` | `invert_x` / `invert_y` | `true` \| `false` only | — | `false` |
| `[camera]` | `authority` | `nextos` \| `native` only (see below) | — | `nextos` |

Numbers use a strict local decimal parser: optional leading `-`, digits,
at most one `.` with digits on both sides. No `strtod` (no locale
decimal-point surprises), no hex (`0x1p2`), no exponent (`1e2`), no
`nan`/`inf`, no empty value. Malformed or out-of-bounds → NXI1002; unknown
tuning key → NXI1001; the same key twice in its section → NXI1003. As
always, any violation rejects the WHOLE file.

### Authority semantics (never double-apply)

Exactly ONE side shapes the camera axes:

- `authority = nextos` (default): NextOS applies deadzone, response curve,
  sensitivity and inversion; the shaped axes replace the raw ones on the
  way into the game.
- `authority = native`: the game's own options menu governs. NextOS passes
  the axes through RAW — deadzone treated as 0, no curve, no sensitivity,
  no inversion — so the user tunes in the native menu only.

**NextOS and native sensitivity are never multiplied together.** Applying
both stacks deadzone-on-deadzone and gain-on-gain and was the source of the
"cursor crawls / camera whips" class of bugs. Pick the authority, once.

### Kinematics API and FPS-invariance guarantee

`include/nxinput_gptk_motion.h` (impl `src/nxinput_gptk_motion.c`, needs
`-lm`):

- `nxinput_gptk_cursor_step()` advances a caller-owned cursor state by one
  frame: radial deadzone with rescaling → response curve → acceleration
  gain → pixels/s from the drawable height → exponential smoothing by
  `smoothing_ms` → trapezoidal integration → clamp into the drawable.
  It is **delta-time based**: integrating a constant deflection over the
  same wall-clock time at 30, 60 or 120 FPS lands within 2% (the host
  gate measures ~0.2%). Ports never need per-FPS tuning values.
- `nxinput_gptk_cursor_state_reset()` places the cursor and zeroes the
  smoothed velocity. Call it on every context switch so residual smoothing
  never drifts into the new context.
- `nxinput_gptk_camera_transform()` shapes one camera sample. Pure
  function, no static state: call it exactly once per sample and feed the
  game the RESULT instead of the raw axes.
- Both fail closed (`-1`, no motion / zero output) on NULL arguments or a
  tuning outside the documented bounds — a memset-cleared struct from a
  failed parse moves nothing.
- `nxinput_gptk_cursor_tuning_get()` / `nxinput_gptk_camera_tuning_get()`
  always return a fully-populated tuning (defaults where unset) plus a
  `*_set` flag per key telling whether the file set it explicitly.

## NEXTOS_CONTROLLERS/2 — o pad inteiro, sempre visível (V4-CONTROLLERS-03/C4)

**V2 é opt-in por port.** A magia sozinha escolhe o schema — não existe
promoção por heurística — e `format = NEXTOS_CONTROLLERS/1` continua literal
para todo port já publicado.

```text
format = NEXTOS_CONTROLLERS/2
```

### 1. Completude

Toda seção presente lista **os 18 controles**, cada um exatamente uma vez, na
ordem estável:

```text
A B X Y L1 R1 L2 R2 L3 R3 START SELECT
UP DOWN LEFT RIGHT LEFT_STICK RIGHT_STICK
```

Campo ausente é **erro** (`NXI1002`, com o nome do controle e da seção na
mensagem). O dono enxerga o pad inteiro mesmo quando o jogo não usa metade
dele. Em V1 a ausência mantém o significado antigo, para não mexer no que já
está publicado.

### 2. Tri-state

| valor | decisão | significado |
|---|---|---|
| `acao.valida` | `ACTION` | governado pelo GPTK e entregue ao sink daquela ação |
| `null` | `SUPPRESS` | **desabilitado explicitamente**, suprimido em todos os caminhos do jogo/consumer |
| `native` | `NATIVE` | passthrough deliberado para um uso nativo declarado pelo adapter |

`null` **não** é “ação vazia com fallback raw”. Ele produz uma decisão
explícita `SUPPRESS`, consumida **antes** de qualquer fallback do dispatcher —
inclusive antes da fonte de fallback estreita (`feed_source`), que não pode
ressuscitar um controle desligado. A autoridade é **por controle e por
contexto**, resolvida por **uma única chamada** (`nxinput_gptk_decide`), para
que nenhum caminho leia duas vezes e discorde.

`null`, `native` e suas quase-grafias são case-**sensitive**: `NULL`, `Null`,
`none`, `nil`, `off`, `disabled`, `Native` e `NATIVE` falham fechados com
mensagem própria. Um desligamento escrito errado nunca vira um desligamento
silencioso.

### 3. Fronteira de lifecycle (fora de banda)

O chord `SELECT+START` é **fronteira do framework**, não consumer e não ação
de jogo. `SELECT = null` ou `START = null` suprime esses botões **para o
jogo** e **não desarma** o chord soberano de saída, que não lê este arquivo
(`nxinput_exit_chord.h` recebe estado SELECT/START já normalizado). O chord
não é remapeável pelo V2 e **nunca** pode usar L2/R2, GUID/layout, fallback
ou botão substituto — provado por gate estático e por teste em runtime.
`null` continua suprimindo qualquer chord que pertença ao **jogo**.

### 4. Analógicos e gatilhos

`LEFT_STICK` e `RIGHT_STICK` são **vetores**, nunca quatro botões inventados;
o adapter declara sinks compatíveis com vetor. Deadzone radial, curva,
smoothing e tempo por frame seguem no contrato de `[cursor]`/`[camera]`.

L2/R2 aceitam eixo contínuo **e** produzem uma borda digital derivada com
limiares distintos de entrada e saída (`0.60` / `0.40`), sem repetição por
frame entre eles. O consumer que quer a natureza analógica registra um
**vector sink** para a ação do gatilho e recebe a magnitude em toda chamada de
`nxinput_gptk_dispatcher_feed_trigger()`. Um gatilho `null` não entrega nada
em nenhum dos dois caminhos.

### 5. Arquivo do dono e upgrade

`defaults/NEXTOSCONTROLLERS.gptk` é o default imutável. A cópia editável do
dono **nunca** é sobrescrita: um default novo chega como
`NEXTOSCONTROLLERS.gptk.new` ao lado, escrito atomicamente
(`openat`+`renameat` sobre descritor de diretório, `O_NOFOLLOW`), com diff
semântico limitado e **sem caminho pessoal**. Adoção é ato humano.
`nxinput_gptk_upgrade_offer_at()` recusa, de forma visível, um arquivo do dono
que seja symlink ou não-regular e um `.new` preexistente que seja symlink — e
nunca escreve através de um link para fora da raiz autorizada.

### 6. Limite de claim

O núcleo V2 é provado **hermético** (`CORE_HERMETIC`): parser, loader,
dispatcher e um consumer realista de teste. Que SDL, Android, Unity, touch ou
API raw respeitem `SUPPRESS` **não** é provado aqui.

Para **Godot 3 e Godot 4** isso passou a ser provado na C5B, em outro lugar:
a costura `nxinput_godot_seam` é compilada e ligada no binário da engine e
decide dentro de `JoypadLinux::open_joypad()`, antes do anúncio do pad. Um
controle marcado `null` no arquivo do dono não chega a InputMap, a `_input`
nem ao polling da engine real — ver `REGRESSION-MATRIX.md`, seção C5B. As
demais engines seguem `PENDING`.


## NEXTOS_CONTROLLERS/3 — FACE_LAYOUT (nxinput 0.10.0)

```ini
format = NEXTOS_CONTROLLERS/3
port = tearscape
FACE_LAYOUT = auto
```

O V3 herda **toda** a completude e o tri-state do V2 e acrescenta exatamente
uma linha obrigatória de preâmbulo, `FACE_LAYOUT = auto|modern|retro`:

- chave e valor em caixa exata; `AUTO`, `Modern`, `face_layout` etc. falham
  fechados com mensagem própria (a doutrina do `null`/`native`);
- exatamente uma ocorrência, fora das seções, depois do magic; ausência,
  duplicata ou posição errada é erro;
- **`FACE_LAYOUT` nunca ultrapassa as autoridades 1/2** da ordem soberana de
  mapping físico. Ele seleciona apenas QUAL variante do bundle do próprio
  port (autoridade 3) pode ser consultada quando nenhuma fonte viva
  responde: `auto` consulta somente `controllers.nxb` (a base invariante,
  que jamais congela uma linha modern/retro mutável), `modern`/`retro`
  consultam `controllers-modern.nxb`/`controllers-retro.nxb`;
- todo arquivo V1/V2 existente permanece byte- e semântica-idêntico e
  equivale a `auto`; nenhum campo V3 é tolerado em schema 1/2;
- o upgrade do dono reporta a diferença de `FACE_LAYOUT` no diff da oferta
  `.new`, sem jamais sobrescrever a cópia editada;
- o runtime vivo capaz de V3 publica o marcador `nxinput-gptk-runtime/3`
  (nunca reutiliza o `/2`);
- a leitura acontece UMA vez, pré-init (`nxinput_gptk_preinit_load()`),
  antes de declarar bundle, staging e `SDL_Init`; o mesmo mapa/receipt
  alimenta o runtime inteiro.


## Limits (fail closed)

| Limit | Value | Violation |
|---|---|---|
| Input size | 65536 bytes | NXI1004 |
| Line count | 512 lines | NXI1004 |
| Action length | 64 chars | NXI1002 |
| Port id length | 64 chars | NXI1002 |
| Encoding | valid UTF-8; no byte < 0x20 except `\n` `\r` `\t` | NXI1005 |
| Dispatcher sinks | 64 registrations | register returns -1 |

Every violation rejects the WHOLE file with a stable code, and the output
structure is cleared: nothing from a rejected file ever reaches the runtime.

## NXI error codes

| Code | Meaning |
|---|---|
| NXI1001 | Unknown name: control token (including any digits-only/evdev token), section, unknown tuning key (or a tuning key outside its section), or — from `nxinput_gptk_validate_actions` — an action absent from the adapter allowlist |
| NXI1002 | Malformed line: missing `=`, empty key/value, invalid action or port syntax, mapping before a section, missing required `[menu]`/`[gameplay]` section, malformed or out-of-bounds tuning number, invalid boolean (only `true`\|`false`), or invalid authority (only `nextos`\|`native`) |
| NXI1003 | Duplicate: same control twice in one section, a second `port` line, or the same tuning key twice |
| NXI1004 | Too large: input over 65536 bytes or over 512 lines |
| NXI1005 | Invalid bytes: broken UTF-8, embedded NUL, or a forbidden control byte |
| NXI1006 | Missing or wrong magic (`format = NEXTOS_CONTROLLERS/1` must be the first non-comment line) — this is also what a gptokeyb file hits |
| NXI1007 | Owner/default I/O boundary: missing, unreadable, symlink, non-regular file or invalid directory descriptor |

Codes are returned as positive integers (`1004` = NXI1004) with a
`"NXI####: reason"` message in the caller's error buffer. Codes are part of
the contract and never renumbered (schema golden rule: a new member never
breaks a published launcher).

## Owner state

The launcher ships a default `NEXTOSCONTROLLERS.gptk` next to the game. Once
the user edits that copy it becomes owner state:

- It is NEVER overwritten by an update, reinstall or repack.
- When a port update carries new defaults, they are written as
  `NEXTOSCONTROLLERS.gptk.new` beside the user's file, and the user (or a
  merge tool acting on the user's behalf) decides what to take.
- A file that fails to parse is not "repaired" in place: the launcher falls
  back to its built-in defaults for the session and reports the NXI code.

The runtime implementation of that promise is
`nxinput_gptk_load_at(owner_dir_fd, defaults_dir_fd, allowlist, ...)`. Both
directories are caller-opened; inside them the loader opens only the fixed
basename with `O_NOFOLLOW`, accepts only a regular file and reads at most
65536 bytes. It validates the immutable default before the owner. A rejected
owner is never opened for writing and is never renamed, truncated or removed.

The bounded receipt `nxinput-gptk-load-evidence/1` names one of `owner`,
`default_owner_missing`, `default_owner_rejected` or `none`; records byte
counts, error codes/messages and SHA-256 of the selected/default bytes (plus
the owner when safely readable); and contains no paths or controller identity.
`source=none` is terminal: without a valid default no mapping reaches runtime.

## What this format is NOT

- **Not gptokeyb.** There is no keyboard target in the vocabulary and no
  `back = esc`-style line will ever parse (no magic → NXI1006). Controller
  input is mapped to semantic actions, never to synthetic key events.
- **Not an evdev table.** Physical identity comes from the SDL/firmware
  controller mapping already consumed by nxinput; this file only speaks the
  symbolic layer above it.
- **Not a scripting language.** No conditionals, no includes, no macros, no
  command execution.

## Vetor X/Y integrado ao dispatcher (V3, blocker 7)

O caminho analógico cursor/câmera é parte do **dispatcher**, não um helper
solto. Fluxo: `nxinput_gptk_dispatcher_feed_stick(d, LEFT_STICK|RIGHT_STICK,
x, y, dt)` → se o stick estiver mapeado a `cursor.*` no contexto atual, o
vetor passa pela cinemática (`nxinput_gptk_cursor_step`, tuning do `[cursor]`
do arquivo, drawable configurado por `nxinput_gptk_dispatcher_configure_motion`)
e o sink vetorial recebe a **posição absoluta** do cursor; se mapeado a
`camera.*`, passa por `nxinput_gptk_camera_transform` e o sink recebe os eixos
moldados. A troca de contexto zera a velocidade/smoothing acumulados.

**Guardas de dupla leitura:**

- `nxinput_gptk_dispatcher_suppressed_mask()` é o ownership do guest por stick:
  marca somente o stick que o contexto vivo entrega a `cursor.*`/`camera.*`.
  A, D-pad, R3 e o outro stick permanecem fora da máscara.
- `nxinput_gptk_dispatcher_set_primary_mask()` é a autoridade de aquisição por
  controle: controles cobertos pelo mapping completo SDL2/SDL3/PortMaster
  ignoram a fonte FALLBACK. Para controle não coberto, `feed_source()` combina
  os estados PRIMARY/FALLBACK por OR, produzindo uma única entrega de press e
  uma única release, sem dupla leitura nem release prematura.

O cursor portátil usa RIGHT_STICK com deadzone radial, resposta progressiva,
suavização e delta-time; R3 entrega `cursor.click` no menu. A troca para
gameplay muda o mesmo stick para `camera.*` e R3 para a ação declarada do jogo.
O mapping não autoriza roubar A/Cross ou D-pad. SELECT+START não passa por esse
map: `nxinput_exit_chord` consulta estado lógico por pad em callback neutro
SDL2/SDL3 e mantém o evdev como fallback independente.

## Runtime vivo obrigatório (TEARSCAPE-CONTROLS-LIVE, 30/08/2026)

REGRA PERMANENTE: um NEXTOSCONTROLLERS.gptk que se apresenta como editável é
um RUNTIME, nunca documentação. A cadeia obrigatória é:

  controle físico → nxinput normalizado → GPTK decide (ação/null/native, por
  controle e por contexto, via `nxinput_gptk_decide`) → dispatcher → sink
  REAL do engine (na linha Godot: `InputEventAction` da InputMap, entregue
  por `Input::parse_input_event`).

- Carregamento SÓ por `nxinput_gptk_load_at` (owner/default, symlink-safe,
  allowlist exata do adapter-contract, recibo limitado).
- Controle governado (ação ou `null`) é SUPRIMIDO antes do caminho nativo —
  uma pressão física nunca dispara duas vezes.
- Troca de contexto solta toda ação latched (`dispatcher_set_context`).
- SELECT+START permanece soberano, fora do arquivo, lido ANTES do GPTK.
- Nada de teclado, gptokeyb, SDL privada ou códigos evdev.
- Opt-in declarativo: `controls.runtime_mapping = "nxinput-gptk"` no
  nxproject (nxgenerator 0.3.8). O nxrelease 0.3.10 falha um candidato que
  declare sem linkar o runtime (marcador `nxinput-gptk-runtime/1` no ELF à
  época; o runtime vivo atual grava `nxinput-gptk-runtime/3` — nxinput 0.10.x,
  o único aceito pelo nxrelease 0.3.25 para candidatos de schema 3) e
  falha um candidato que embarque o arquivo "editável" sem declarar.
- Gate permanente: `tests/test_gptk_live_remap.c` (editar uma ação muda o
  sink chamado; troca de contexto solta o latch) roda no run-gptk-host.sh.
- Ports antigos publicados não migram automaticamente; cada um adota o modo
  no seu próximo bump, com prova própria.

Primeira integração real: Tearscape (glue
`nxinput_gptk_godot.cpp` no driver SDL do engine, contexto menu/gameplay por
foco de GUI, sticks analógicos preservados por strength de ação).
