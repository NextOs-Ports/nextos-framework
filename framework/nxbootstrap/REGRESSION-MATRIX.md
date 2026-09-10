# Matriz de regressão do nxbootstrap 0.8.4

| Contrato | Prova automática | Resultado exigido |
|---|---|---|
| Ciclo do arquivo do dono, no launcher REAL (E6a) | `test-owner-e6a-real.sh` | launcher renderizado por `generate-port.py` (schema v3, `--runtime-root`, bytes/digests reais) aberto QUATRO vezes: semeia uma vez de `defaults/` e o jogo recebe o valor; dono edita + default novo → bytes do dono intactos, `.new` oferecido e a edição chega ao estado efetivo sem rebuild/repack; TERCEIRO default → `.new` atualizado com receipt; `.new` editado pelo dono → preservado e nomeado, nunca sobrescrito; `port-env.sh` ausente do manifesto renderizado |
| Biblioteca privada sob NXExtract | `test-manifest-contract.py` + `test-generation-v2.sh` | `private-library` 0644 estritamente abaixo de uma raiz declarada em `nxextract/` entra na closure/store/heal somente com NXExtract ativo; modo `no`, fora/igual à raiz, 0755, path canônico reservado e owner data falham; helper ELF não é reclassificado nem autorizado em 0644 |
| Provider SDL de sistema | `test-generator.sh` + `test-launcher-behavior.sh` | `sdl_provider: system` é canônico e fechado; firmware precede overlays PortMaster, inherited path dentro do port é removido, path externo sobrevive, resolução exata é registrada e `SDL_VIDEODRIVER` não é definido |
| Captura precoce do provider | `test-generator.sh` + `test-launcher-behavior.sh` | `BIN_PRELOAD`/`LD_PRELOAD`/`SDL_DYNAMIC_API` herdados são capturados/unset com builtins antes do primeiro subprocess/helper executado; depois do hook top-level os valores são copiados, de-exportados e removidos; readonly herdado falha com `launcher-error` 0600 e nenhum helper, readonly do hook falha com cleanup/lock reutilizável; overrides aceitos alcançam somente o subshell do `exec` |
| Overrides do provider SDL | `test-launcher-behavior.sh` | entradas não resolvidas falham fechado; core e nove famílias de add-ons package-private renomeadas/content-detectable falham antes do exec; overrides resolvidos fora do GAMEDIR, inclusive SDL_DYNAMIC_API e fingerprint SDL, preservam autoridade externa e chegam somente ao filho; `BIN`, interpreter, game loader e library route não podem ser substituídos pelo hook |
| Token `$LIB` do loader | `test-generator.sh` + `test-launcher-behavior.sh` | `LD_PRELOAD` herdado com `$LIB` ou `${LIB}` e expansão externa, regular e única em `/proc/$$/maps` preserva o literal; uma entrada SDL1/2 diretamente sob `/usr` ou `/lib`, herdada, não mapeada e não alterada pelo adapter é removida nominalmente do ambiente efetivo; filho e helpers ficam sem ela; caminho privado, basename não SDL, subdiretório extra, valor criado pelo adapter, token em outra variável ou ambiguidade falha fechado, sem `eval` nem palpite `lib/lib64` |
| Compatibilidade sem opt-in SDL | `test-generator.sh` | campo ausente preserva ordem PortMaster/firmware, sufixo herdado, hook e launch block legados, sem captura/imutabilidade system-provider; valor diferente de `system` falha fechado |
| Provider SDL1/2 privado | `test-launcher-behavior.sh` | nomes de core e add-ons image/mixer/ttf/net/gfx/gpu/sound/rtf/fontcache são recusados antes do exec independentemente do campo; SDL3 não é confundida com SDL1/2 e segue para o gate formal do NXRelease; inspeção profunda package-wide permanece no NXRelease |
| Preto/dead-context fatal | `test-launcher-behavior.sh` | receipt run-bound 0600 BLACK conclusivo encerra por PID/starttime o filho que continua vivo e produz áudio; filho que ignora TERM chega ao KILL somente após revalidação imediata e sentinela mantém PID/starttime; health é removido, `NXR0004` é emitido e status é 72 |
| Tuple health imutável | `test-launcher-behavior.sh` | assign e unset dos seis campos no `port-env.sh` falham como readonly; os valores originais chegam ao child e o path não é redirecionado |
| Vídeo obrigatório para health | `test-generation-v2.sh` | com `video_proof: required`, exit 0, PID, áudio, render-ready e health sem receipt OK não promovem; OK observado e depois malformado também não promove; somente o receipt final atual `OK/non-black` promove |
| Status do filho na promoção | `test-generation-v2.sh` | health `ready` e VIDEO `OK/non-black` exatos com child status 1 preservam a geração pendente, incrementam `prehealth_failures`, emitem `NXU0005`, não emitem `NXU0006` e devolvem status 1 |
| Crash real e rollback A/B | `test-v4-rollback-real-crash.sh` | A saudável é promovida; B recebe `SIGABRT` 134, `SIGTERM` 143 e saída 42 antes de health; exatamente na abertura seguinte o estado persistido e o runtime voltam para A |
| Concorrência A/B e identidade da tentativa | `test-v4-rollback-real-crash.sh` | o runtime A permanece vivo enquanto cada arquivo top-level da closure é substituído atomicamente por B; o launcher B se sobrepõe, sai não zero e não cria filho; PID, PPID, starttime, run-id e status permanecem coerentes e A/B nunca ficam vivos juntos |
| Receipt stale, late e PID reciclado | `test-v4-rollback-real-crash.sh` | receipt de outro run, receipt escrito após a morte exata do launcher e mesmo PID com starttime/run-id divergentes são recusados; uma reabertura real confirma que nenhum deles promove a pendência |
| Âncora e persistência de rollback | `test-v4-rollback-real-crash.sh` | `previous_healthy` ausente ou sem `commit` falha fechado após o orçamento; bind mount somente-leitura impede a persistência, retorna não zero e não executa A nem B |
| Instalação fresca sem receipt | `test-v3-generations.sh` | três aberturas limpas sem emissor promovem/mantêm a única geração, não armam verificação profunda, preservam arquivo extra do dono e nunca emitem `NXU0009` |
| Estado fresco anteriormente bloqueado | `test-v3-generations.sh` | `active=null`, `previous_healthy=null` e três falhas emitem `NXU0015`, executam a única geração completa e promovem após saída limpa; nenhuma âncora é fabricada |
| Deadline, recibo e higiene | `test-v4-rollback-real-crash.sh` | cada tentativa usa deadline monotônica derivada de `/proc/uptime` e recibo estruturado com identidades/status, schema, `activation_seq`, `last_health_run_id`, transição e hashes; lock é reclamável, fallback/staging/temporários/processos não sobrevivem e manifesto SHA-256 prova owner-data inalterado |
| Espaço insuficiente recusa | `test-v4-bundle.sh` | tmpfs pequeno REAL: `NXU0013`, nenhuma geração criada, nenhum staging, jogo nunca alcançado, owner-data intacto; controle positivo no mesmo tamanho suficiente |
| ENOSPC não desabilita o ativo | `test-v4-bundle.sh` | update sem espaço é recusado, cai de volta na geração ativa, o jogo roda, `active` não muda; com espaço livre o MESMO seed completa o update |
| Promoção impublicável | `test-v4-bundle.sh` | store somente-leitura por bind mount (não por `chmod`, que root atravessa): recusa, staging removido, nenhuma geração parcial, jogo nunca alcançado |
| Seed visível `.nxb` | `test-v4-bundle.sh` | arquivo regular 0644 determinístico, sem `commit`, com a closure completa; regerar produz bytes idênticos |
| ZIP pessoal sem dotdir | `test-v4-bundle.sh` | Info-ZIP, 7-Zip e ZIP DOS/stored/embaralhado instalam e abrem; `gamedata` preservado byte a byte |
| Cache reconstruível | `test-v4-bundle.sh` | apagar `.nxruntime` inteiro é recuperável somente pelo seed autenticado |
| Materialização transacional | `test-v4-bundle.sh` | staging privado, `commit` por último, promoção atômica, staging nunca sobrevive |
| Raiz de confiança do seed | `test-v4-bundle.sh` + `BUNDLE-TRUST-V4.md` | `sha256(identity.json)` = id compilado; launcher instalado tem de pertencer à closure |
| Negativos do seed | `test-v4-bundle.sh` | ausente, truncado, adulterado, cabeçalho reescrito, geração estrangeira, traversal, path absoluto, duplicata, membro extra/ausente, contagem errada, offset deslocado, modo inválido, FIFO e symlink falham fechado sem chegar ao jogo |
| Geração nunca reparada | `test-generation-v2.sh` + `test-v4-bundle.sh` | `commit` ausente continua `NXU0002`/`NXU0009`; o seed não o recria |
| Preflight de espaço | `test-v4-bundle.sh` | recibo `STORAGE:` presente; `NXU0013` recusa antes de qualquer escrita |
| `setrlimit` fail-closed | `test-namespace-watchdog.sh` | limite exigido que não pode ser estabelecido recusa o filho em vez de rodá-lo sem cerca |
| Schema v3 opt-in | `test-manifest-contract.py` | `generation_runtime` não vazio, ordenado e exato; v1/v2 preservam o scaffold legado |
| Runtime closure | `test-generation-v2.sh` | launcher, nxport, NXSplash, ELF, libs, hooks e closure NXExtract completa ficam hash/modo/papel/path-bound sob `files/runtime` |
| Dados de runtime gerenciado | `test-manifest-contract.py` | `runtime-data` aceita somente `0644` abaixo de `private_library_paths`; path externo e modo executável falham fechado |
| Closure NXExtract | `test-manifest-contract.py` + `test-generation-v2.sh` | receita, engine, runner, runtime-env, UI e todo helper/spec declarado pertencem à mesma geração; core ausente, path/modo errado ou arquivo extra falha antes da extração |
| Cura all-or-nothing | `test-generation-v2.sh` | update root deliberadamente híbrido cura integralmente; falha tardia de staging não publica nenhum membro; temporário de interrupção com PID morto é recuperado somente dentro de NXExtract e sentinel fora é preservado; nenhum caso chega a NXExtract/jogo antes da closure exata |
| A/B e rollback | `test-generation-v2.sh` | A e B promovem por receipt; helper NXExtract torn em B volta à A v2 sem runtime híbrido |
| Symlink e revalidação | `test-generation-v2.sh` | symlinks top-level são substituídos sem tocar o alvo externo; toda closure é relida antes do jogo |
| Compatibilidade v1/v2 | `test-v3-generations.sh` + `test-generation-v2.sh` | v1 segue control-only e nunca ancora pending v2 |
| SHA obrigatório | `test-generation-v2.sh` | `sha256sum` ausente/inutilizável recusa qualquer generation-v2 |
| Persistência da promoção | `test-generation-v2.sh` | rename de state falho emite NXU0011/status 70 e jamais NXU0006 |
| PortMaster em POSIX | `test-generation-v2.sh` | fixture reproduz `chmod -R 777`; identidade/path/tipo/SHA/closure são validados antes de qualquer chmod, store é restaurado a `0644`/`0755` autenticados e a cura live exige igualdade |
| Filesystem chmodless | `test-generation-v2.sh` | fixture 0777/chmod ignorado passa somente após probe; hash/symlink/exec continuam fechados |
| NXExtract desativado | `test-manifest-contract.py` | nenhuma fase ou UI NXExtract |
| NXExtract ativo completo | `test-generator.sh` | UI regular, não vazia e executável antes da extração |
| Pin NXExtract | schema, gerador, registro e contrato | schema/manifest novo fixa a canônica 1.2.21; o gerador preserva somente pins imutáveis 1.2.14–1.2.21 ainda presentes no registro para ports já opt-in |
| Topologia mixed ABI | `test_mixed_abi.py` | extractor AArch64, splash ARMHF e jogo ARMHF permanecem papéis independentes |
| ELF do papel | `test_mixed_abi.py` | classe e `e_machine` divergentes falham fechado |
| Loader alternativo | `test_mixed_abi.py` | ausente, arquivo não ELF e ABI errada são rejeitados; somente candidato ARMHF coerente é selecionado |
| Closure ARMHF | `test_mixed_abi.py` + ambiente Spruce 1.1.0 | chroot/`usr/lib32` aceitos; `muOS/usr/lib` AArch64 não entra no caminho |
| Compatibilidade sem opt-in | `test-068-preservation.py` | exemplos históricos não recebem funções/receipts mixed ABI e preservam os pisos cumulativos |
| Resultado terminal NXExtract | `test-generator.sh` + `test-phase-observability.sh` | JSON regular/único/schema v1/outcome coerente; resumo canônico copiado antes da próxima fase |
| UI ausente ou integração parcial | `test-generator.sh` | falha fechada antes do loader |
| Pin NXSplash | manifesto, gerador e `test-068-preservation.py` | versão exata 0.1.2 e artefato correspondente à arquitetura |
| Logo NEXTOS em toda abertura | `test-generator.sh` | handoff único de 5000 ms depois do payload |
| Ordem nativa | `test-generator.sh` | NXExtract/UI → payload → logo → adapter → jogo |
| Core histórico do launcher | `test-068-preservation.py` | runtime aposentado preservado por hash; contratos seguros 0.6.8 continuam presentes no launcher 0.6.27 |
| Fronteiras de fase | `test-phase-observability.sh` | preflight, NXExtract, NXSplash, runtime, provider e primeiro frame em ordem; sete cortes preservam a última evidência e o status real |
| Publicação atômica | schema + `test-phase-observability.sh` | `nxphase-result.json` nunca é observado parcial e não deixa temporários |
| Falha pré-log | `test-launcher-behavior.sh` | trap anterior à descoberta, `umask 077`, PID exclusivo, fallback para runtime e `pm_finish` uma vez |
| Observação segura de CFW 0.6.8 | `test-068-preservation.py` + `test-launcher-behavior.sh` | `.OS` limitada/regular, dArkOSRE com dois marcadores e nome hostil recusado |
| Shell sem `stat` externo | `test-safety-static.sh` e firmware gates | launcher inicia e diagnostica normalmente |
| Instância única sem `flock` | `test-launcher-behavior.sh` em namespace privado | primeira execução mantém owner `PID/token`; segunda sai 1 sem child e sem remover/alterar o lock; owner adulterado nunca é liberado |
| Bundle de suporte | `test-safety-static.sh` | launcher pede somente bundle nxobs sanitizado e nunca instrui envio de logs crus |
| PortMaster e finalização | `test-launcher-behavior.sh` | `pm_finish` exatamente uma vez |
| UI de apresentação NXExtract 1.2.16 em NextOS Mali-450 | recibo físico isolado | `visible=sdl` via `mali`, captura gráfica válida inspecionada e nenhum processo residual; engine 1.2.21 preserva esses pixels |
| Renderer de apresentação NXExtract 1.2.16 em alvo ArkOS-class/KMSDRM | recibo físico isolado | `visible=sdl`, log KMSDRM após recuperação EGL/GLES e nenhum processo residual; engine 1.2.21 preserva layout e renderer |

As provas são cumulativas. Uma versão posterior não pode remover a UI do kit,
transformar a logo em opt-out ou mover qualquer uma das duas após o jogo.
| Teto do log de runtime | `test-launcher-behavior.sh` | execução comum nunca é cortada; 6 MiB de saída caem para <= 4 MiB guardando a cauda, com a linha do corte e o recibo final de status intactos |
| Teto do arquivo de eventos | `test-launcher-behavior.sh` | > 1 MiB de eventos cai para <= 1 MiB; toda linha continua JSON válido, a primeira é o registro `NXU0014` com `dropped_bytes`, a cauda sobrevive e a cabeça não |

## 0.7.5 — boot fast path (V4-BOOT-01/02/03)

| Caso | Gate |
| --- | --- |
| Primeira abertura normal (sem estado) rápida, sem caminho profundo | tests/test-boot-fastpath.sh |
| Segunda abertura (estado ativo) rápida, sem caminho profundo | tests/test-boot-fastpath.sh |
| Subprocessos constantes: closure 8 = closure 4205 (chmod/rm/ls/sha) | tests/test-boot-fastpath.sh |
| Launcher→preflight < 1 s no host com 4205 membros chmodless | tests/test-boot-fastpath.sh |
| Probe chmodless no máximo uma vez por execução | tests/test-boot-fastpath.sh |
| Recuperação (nxport adulterado): NXU0014 visível + heal + sha em LOTE | tests/test-boot-fastpath.sh |
| Pós-recuperação volta ao fast path | tests/test-boot-fastpath.sh |
| Commit ausente: launch recusado (NXU0009) | tests/test-boot-fastpath.sh |
| Entry point symlink: nunca fast path | tests/test-boot-fastpath.sh |
| Entry point adulterado: health falha e o boot seguinte cura (NXU0003) | tests/test-boot-fastpath.sh |
