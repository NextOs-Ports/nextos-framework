# nxdoctor

SPDX-License-Identifier: GPL-3.0-only · versão/version 0.3.0 · Python 3.8+, stdlib only

## PT-BR

`nxdoctor.py <dir-do-port> [--json]` é o diagnosticador **somente-leitura** do
layout V3/V4 (`.nxruntime/`). A execução padrão **não abre nenhum arquivo para
escrita**: ela apenas relata.

Escopo da evidência V4: diferencie sempre diagnóstico read-only (que apenas observa),
ação explícita (modifica estado a pedido), prova host (fixture sintética validando regras)
e prova física. Esta versão **não possui uma âncora externa de confiança** e,
portanto, rejeita `PHYSICAL` explicitamente: essa classe permanece `PENDING`.
**Nunca** afirme recuperação PHYSICAL, compatibilidade de firmware ou
universalidade com um envelope SHA autocriado.

O relatório cobre:

- **Estado**: suporta schemas v1 e v2. Relata `active` / `pending` / `previous_healthy`
  (normalizando o literal `"null"` do launcher para ausente), `activation_seq`,
  `prehealth_failures` e, no v2, `failure_generation` / `last_health_run_id` —
  ou `legacy: no generation state` quando não há `state.json`.
- **Gerações**: suporta ids de 32 hex (antigo) e 64 hex (SHA-256 completo). Exige
  marcador `commit` e manifest estrito (`nxruntime-generation-v1` ou `v2`, onde
  v2 mantém os bytes sob `files/`). Presença + sha256 + modo de cada componente →
  `complete` / `incomplete` / `corrupt`. Em mídia chmodless (FAT/exFAT), o modo real
  ser diferente do fixado torna-se observação documentada em vez de corrupção fatal.
  **Nunca** decide por mtime/ctime (V3-CLOCK-01).
- **Espaço**: bytes livres, tamanho de `.nxruntime/staging*`, lista de gerações
  não referenciadas e do **seed visível** (`nxruntime-<id>.nxb`).
- **Dados do dono**: existência de `gamedata/` e `gamedata/README.txt`.
- **Sintaxe**: `NEXTOSCONTROLLERS.gptk` e `NEXTOSSETTINGS.txt`. Relata
  ok/invalid+motivo; **jamais reescreve**.
- **Locks**: lê tanto o owner histórico `PID starttime` quanto o owner real do
  launcher base `pid=PID token=RUN_ID`, sem expor o token (somente hash).
- **Logs**: inventário.
- **Fase**: lê e observa o limite de `nxphase-result.json`.

### Ações (cada uma exige a própria flag; todas idempotentes; recusam symlink; recibo JSON no stdout)

Qualquer falha do filesystem (permissão negada, diretório ocupado) não solta traceback:
é emitido um recibo gracefully fechado listando o arquivo que travou.

| Flag | Efeito | Recusa quando |
| --- | --- | --- |
| `--restore-previous --generation <id>` | reescreve `state.json` atomicamente (`active=<id>`, `pending=null`, `activation_seq+1`) | sem `state.json`, id ≠ `previous_healthy`, geração não-completa |
| `--discard-staging --path <rel>` | desanexa atomicamente um caminho dentro de `.nxruntime/staging*` e o limpa; falha tardia fica em quarentena explícita | fora do staging, traversal, symlink, hardlink>1, geração commitada, padrão de dado do dono |
| `--complete-gc --generation <id>` | remove `.nxruntime/generations/<id>` (tira o `commit` primeiro, deixando claim autenticada e retomável) | id inválido, referenciada por qualquer campo do state v2, incompleta (salvo claim exata), symlink, dado do dono |
| `--clear-lock --pid <pid>` | desanexa o dir de lock de fallback e o limpa | owner ausente/inválido/diferente; PID/starttime ainda vivo; ou qualquer PID vivo em owner token-only |

Toda ação adquire o lock canônico `flock` e reserva simultaneamente o namespace
de fallback antes de mutar. Restore usa comparação do state observado; GC
revalida os mesmos bytes e protege também `failure_generation`. `clear-lock`
aceita os dois formatos estritos do owner, só trata `ENOENT/ESRCH` como morto
e desanexa o lock atomicamente antes da limpeza. Um record token-only permite
limpar PID ausente, mas nunca alegar reciclagem de PID vivo. Cada receipt ecoa o alvo.

### Proibições (impostas no código e cobertas pelo gate)

- Sem rede. Sem escrita na execução padrão.
- Nunca apaga `*.apk`/`*.obb`/`save*`/`gamedata/`.
- Nunca reescreve `.gptk`/`NEXTOSSETTINGS.txt`.
- Nenhuma ação implícita, cura automática ou decisão baseada em mtime.

### Kit de evidência 0.3.0

`evidence.py` executa **uma** das quatro ações explícitas e emite
`nx-doctor-recovery-evidence-v1`. O envelope fixa SHA-256 do payload, do
`nxdoctor.py` e do próprio coletor; guarda somente hash/tamanho do stdout e
stderr, mas preserva o recibo JSON parseado. O snapshot canônico registra
estado, gerações, staging, lock, toda a superfície protegida fora de
`.nxruntime` e o subconjunto de dados do dono. Caminhos são relativos;
path absoluto, IP e output cru do processo são recusados; conteúdo de dados do
dono aparece somente por metadados e hashes. O alvo relativo pedido continua
visível porque é parte obrigatória da identidade da ação.

Exemplo host, sempre classificado `HOST_FIXTURE`:

```sh
python3 evidence.py capture /fixture/port \
  --action restore-previous --generation ID --run-id caso-001 \
  --out /evidencias/caso-001.json
python3 evidence.py validate /evidencias/caso-001.json \
  --expected-run-id caso-001 --expected-doctor-sha256 SHA256 \
  --expected-environment HOST_FIXTURE \
  --action restore-previous --generation ID
```

O arquivo de saída deve ser novo, externo ao port e é criado como `0600`, sem
seguir symlink. Sem `--out`, o JSON vai ao stdout.

Não existe flag `--physical`. Os comandos `physical-context`, captura com
contexto físico e validação `--expected-environment PHYSICAL` recusam com
`PENDING`: arquivo+SHA fornecidos pelo mesmo chamador não autenticam hardware.
Uma versão futura só poderá liberar essa classe com atestado/âncora externa
verificável e prova no aparelho autorizado.

## EN

`nxdoctor.py <port-dir> [--json]` is the **read-only** diagnostician for the
V3/V4 on-device layout (`.nxruntime/`). A default run **never opens a file for
writing**: it only reports.

V4 evidence scope: always distinguish read-only diagnosis, explicit action requests,
host proofs (synthetic fixtures validating rules), and physical proofs (real device
execution emitting an authentic receipt). **Never** claim PHYSICAL recovery,
firmware compatibility or universality without an actual evidence receipt emitted
by the target device.

The report covers: state (v1/v2 schemas, normalizing `"null"` to absent);
per-generation health (32/64 hex ids, v1/v2 manifests with `files/` components,
strict component presence + sha256 + mode, where chmodless media mode differences
become notes instead of fatal corruptions, never mtime-based); space (free bytes,
`.nxruntime/staging*` sizes, unreferenced generations, and the **visible seed**
`nxruntime-<id>.nxb`); owner data presence; syntax validation of `.gptk` and
settings; fallback-lock owner liveness; and log/phase inventory. `--json` emits
sanitized schema paths.

Actions (idempotent, require explicit flags/targets, fail-closed on filesystem denials
without tracebacks, JSON receipt):
`--restore-previous --generation <id>` (atomic `state.json` rewrite),
`--discard-staging --path <rel>` (never committed generation/owner data),
`--complete-gc --generation <id>` (safely removes `commit` first, placing a resumable claim),
and `--clear-lock --pid <pid>` (after proving a starttime owner dead/reused, or
a real launcher token owner dead; a live token-only PID is always refused).
GC now requires valid state before mutation, clear-lock requires a valid matching
owner record, and every receipt echoes its requested target.

Prohibitions enforced in code/gate: no network; never delete owner data;
never rewrite configs; no implicit actions, auto-healing, or mtime decisions.

### 0.3.0 evidence kit

`evidence.py` executes exactly one explicit action and emits the versioned
`nx-doctor-recovery-evidence-v1` envelope. It pins the payload, doctor and
collector hashes; records canonical state, generation, staging, fallback-lock,
protected-surface and owner-data snapshots; and retains only process-output
hashes/lengths plus the parsed JSON receipt. Evidence files are new external
`0600` files (or stdout) and are never written inside the port.

Host capture is unconditionally `HOST_FIXTURE`. There is no `--physical`
boolean, and this version rejects physical-context creation/capture and
`--expected-environment PHYSICAL` as `PENDING`. A caller-supplied file plus its
own SHA-256 is not a hardware trust anchor. A later version may enable this
class only with independently verifiable attestation on an authorized device.

## Gates

```sh
python3 -B tests/test_nxdoctor.py
python3 -B tests/test_evidence.py
```
