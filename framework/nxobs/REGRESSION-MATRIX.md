# NXObs regression matrix

| Gate | Rejected regression | Evidence |
|---|---|---|
| PERF metrics complete | threads, open fds, CPU utime/stime and every /proc/self/io counter are read and reported | `tests/run-perf-sampler.sh` |
| PERF absent sources | a missing or truncated /proc/self/io yields a smaller receipt, never a fabricated one, and never suppresses the other sources | `tests/run-perf-sampler.sh` |
| PERF milestones | the first mark is the measurement, a repeat is ignored, an unmarked or reversed interval reports -1 instead of zero | `tests/run-perf-sampler.sh` |
| PERF frame path | present() performs no I/O, no allocation and no logging; a burst of ticks emits at most once per second | `tests/run-perf-sampler.sh` (runtime + static audit) |
| PERF never acts | the sampler contains no kill/setrlimit/malloc_trim/unlink | `tests/run-perf-sampler.sh` static audit |
| VSYNC receipt | requested and effective swap intervals stay separate; a driver that ignored the request reports honored=0 with the measured FPS | `tests/run-perf-sampler.sh` |
| Signal truth | swallowed/relabelled SIGSEGV or SIGABRT | three subprocess deaths assert the original signal |
| Early/late context | missing phase/frame/asset/GL/provider | `test_p06_observability.py` early and frame-42 fixtures |
| Registers/module | missing PC/SP, build-id or relative offset | native receipt + strict schema |
| Privacy | symlink/public runtime dir, mode other than 0600, raw map/source path | runtime and bundle adversarial checks |
| Async boundary | second crash recursion or invented clean exit | one-record handler, `SA_RESETHAND`, re-raise |
| Architecture | source no longer compiles for AArch64 | `aarch64-linux-gnu-gcc -Werror` |
| Symbolization | wrong basename/build-id or absolute source path | `nx-symbolize-crash.py` positive/negative cases |
| Symbol custody | symbols inside public output or overwrite/race | private-root + `RENAME_NOREPLACE` archive |
| Bundle compatibility | 0.1.0 events/logs stop parsing | historical M12A suite |
| Visual/native flow | renderer, splash, NXExtract UI or draw order changes | component has no visual/device code; source-only diff audit |

## 0.4.4 (C6 admission receipts in the shared bundle)

| Gate | Rejected regression | Evidence |
|---|---|---|
| `tests/test_v4_c6_receipt.py` | dropping the NXC6-SEAM/NXC6-DOMAIN evidence, promoting a blocked pad, or letting pid/tid/prose/unlisted fields into the shared bundle | pure |

## 0.4.3 (private device-launch proof directory)

| Garantia | Prova |
|---|---|
| Diretório novo nasce privado | fragmento real `umask 077; mkdir` produz diretório 0700 pertencente ao caller | `tests/test_device_launch_proof_dir.py` |
| Posse exclusiva | diretório pré-existente e symlink são recusados e preservados | `tests/test_device_launch_proof_dir.py` |
| Sem regressão chmod/stat | ausência de `mkdir -p`, `chmod` posterior e comando externo `stat`; cleanup só após posse confirmada | `tests/test_device_launch_proof_dir.py` |

## 0.3.0 (V3)

| Garantia | Prova |
|---|---|
| events.jsonl nativo com run_id atravessando componentes | `tests/test_events_jsonl.py` |
| Linha final rasgada tolerada como diagnóstico; malformada no meio falha fechado | `tests/test_events_jsonl.py` |
| Caminho pessoal/host nunca chega ao bundle vindo do events.jsonl | `tests/test_events_jsonl.py` |
| Versão do componente ≠ versão da ferramenta no relatório | `tests/test_events_jsonl.py` + relatório |
| Budgets de log medidos sem tocar saves/jogo; --strict só para artefato do framework | `tests/test_budget_check.py` |

## 0.3.1 (credential privacy hardening)

| Garantia | Prova |
|---|---|
| Authorization, Api-Key, X-Authentication, Cookie e Set-Cookie estruturados nunca preservam o valor | fixtures sintéticas negativas em `tests/test_m12a_observability.py` e `tests/test_events_jsonl.py` |
| Bearer, JSON serializado e token/key/session/auth em query string viram marcadores finitos | asserts de `redacted-authorization`, `redacted-cookie`, `redacted-credential` e `redacted-query` |
| Evento mantém source/phase/status/reason e detalhes não sensíveis | fixture confere `attempt`, `label` e `recipe` após a redação |
| Host arbitrário e credencial em metadata pública falham fechado | gate de `host` em events.jsonl e run_id sintético adversarial |
| Nenhum segredo real é material de teste | todos os valores têm namespace explícito `nxobs-fixture`/`nxobs-events-fixture` e são verificados ausentes de cada arquivo público |
| Coleta pública não deixa logs crus nem staging | `tools/tests/test-nx-logs-privacy.sh`: default e alias `--bundle` publicam cinco arquivos sanitizados; falhas removem staging |
| TAR hostil é recusado antes de extrair | fixtures herméticas de traversal, membro duplicado e symlink; `network_access=0` |
| Evidência crua exige intenção explícita | `--raw-internal` é incompatível com `--bundle`, marca a saída `INTERNAL-ONLY-DO-NOT-SHARE` e avisa no stderr |

## V4-GRAPHICS-04 (0.4.1)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Receipt final preservado | `GRAPHICS-EVIDENCE` vira evento `graphics/evidence` com campos allowlisted; OK=`ok`, FAIL=`failed` | `tests/test_v4_graphics_evidence.py` |
| Pré-present nunca promove | `GRAPHICS-PREPRESENT-EVIDENCE` vira `graphics/pre-present` OBSERVED; nunca `ok` | `tests/test_v4_graphics_evidence.py` |
| Sanitização | Caminhos de DSO de provider e o path privado da fonte nunca aparecem no bundle publicado | `tests/test_v4_graphics_evidence.py` |

## V4-CONTROLLERS-03 / C2 (0.4.2)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Receipts de input preservados | Os seis NXINPUT-* viram eventos `input/observe-*` OBSERVED com campos allowlisted | `tests/test_v4_input_observe.py` |
| Nunca promover | `pending/not-instrumented` permanece pending; status nunca vira ok | `tests/test_v4_input_observe.py` |
| Allowlist fail-closed | hostname/ip/path e chaves desconhecidas descartadas; fim a fim sem vazamento de path | `tests/test_v4_input_observe.py` |
