# Matriz de regressão do NXRelease 0.4.11

## Composição nxbootstrap 0.8.4

- launcher, manifesto de geração e source pins devem identificar exatamente
  nxbootstrap 0.8.4 e os hashes reais do gerador/template;
- nxbootstrap 0.8.3 e qualquer outra identidade continuam recusados antes do
  stage/ZIP;
- nenhuma SDL1/SDL2 privada ganha exceção: o novo caminho remove somente o
  token SDL de sistema herdado que o loader não mapeou e que o adapter não
  substituiu.

## Composição nxgenerator 0.4.5

- `GENERATION.json` e a raiz real do gerador devem identificar exatamente 0.4.5;
- 0.4.4 e qualquer outra identidade continuam recusadas antes do stage/ZIP;
- o fluxo humano e a abertura única de 0.4.8 permanecem byte-funcionalmente iguais.

## V5 human authority / one-open packaging

- no lock => `human`, zero receipt de device exigido ou embutido;
- lock explícito => preserva a autoridade `candidate-lock` anterior;
- misturar human + lock ou pedir candidate-lock sem arquivo falha;
- `build` e `bundle` reabrem o ZIP final exatamente uma vez;
- helpers schema 3 não repetem essa abertura; legacy/`--skip-build` continuam verificando;
- todos os gates mecânicos de conteúdo e compatibilidade permanecem ativos.

Acréscimos da 0.4.2 (closure de controles e prova de prompt):

| Fronteira | Prova | Resultado exigido |
|---|---|---|
| Closure de controles do port schema 4 | `test_controls_closure_gate.py` + `validate` | `CONTROLS-CLOSURE.json` obrigatório em schema 4, `nx-controls-closure/1`, port certo, contextos declarados, casos de UMA entrega `ACTION` em press/axis/motion; caso em contexto não declarado reprova |
| Prova de prompt | `test_prompt_capture_gate.py` + `validate` | receipt `/1` recusado por identidade; em `/2`, PASS exige região declarada e glyph esperado reconhecido; `raw_ordinal_tokens` numa região derruba o PASS; sem expectativa = INCONCLUSIVE, nunca PASS por silêncio; controle positivo = o receipt `/1` do FP2 na árvore hoje é recusado |

Acréscimos da 0.4.1 (vendor byte a byte):

| Fronteira | Prova | Resultado exigido |
|---|---|---|
| Vendor byte a byte contra o pin | `test_vendor_pin_gate.py` + `validate` | cada arquivo de `ports/<p>/vendor/<componente>/` é comparado com `framework/<componente>/<caminho>`: diferença, contradição com o próprio `PINS.json`, arquivo não declarado, conjunto vazio, componente fora do `FRAMEWORK-PIN.json` e `PINS.json` apagado num diretório de nome de componente reprovam; árvore de terceiros (`sdl2`) não precisa de pin; a divergência viva dos pilotos tem de ser igual a `vendor-drift-v1.json` |

Acréscimos da 0.3.25 (adaptador estrutural nxscan):

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| carga do scanner | nxrelease sem `NXSCAN`, path errado ou schema desconhecido | `tests/test_nxscan.py::WiringTest::test_nxrelease_loaded_the_structural_scanner` |
| decisão estrutural | credencial provada por AST/JSON não chegar ao scanner vivo | `...::test_structural_fail_rejects_through_the_live_scanner` |
| aditividade | `PASS` estrutural absolver o que a regex rejeitava | `...::test_adapter_is_strictly_additive` + mesmo teste acima |
| ganho real | literal Python entre aspas passar (invisível à regex) | `...::test_structural_catches_what_the_regex_cannot` |
| fallback UNSUPPORTED | tipo não parseado deixar de ser decidido pela regex | `...::test_unsupported_falls_back_to_the_regex_authority` |
| fallback STRUCTURAL_ERROR | sintaxe inválida, BOM, duplicata ou NaN virarem PASS | `...::test_structural_error_falls_back_and_stays_fail_closed`, `...::test_bom_duplicate_and_nan_json_fall_back_not_pass` |
| falso positivo de campo | anotação `name: Type` voltar a reprovar | `...::test_python_annotation_false_positive_stays_gone` |
| robustez | crash do scanner virar aprovação | `...::test_scanner_crash_degrades_to_regex_not_to_pass` |
| privacidade | finding ecoar o valor do segredo | `...::test_no_finding_echoes_a_secret_value` |
| autoridade viva | regressão do scanner textual existente | `tests/test_secret_literal_scan.py` |

# Matriz de regressão do NXRelease 0.3.24

Acréscimos da 0.3.24 (GPTK V3 / FACE_LAYOUT):

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| default V3 | aceitar V3 sem FACE_LAYOUT único/minúsculo, ou V1/V2 com o campo | `tests/test_face_layout_release.py` (contrato de fonte) |
| par de variantes | variante única, renomeada, hash divergente, symlink, vazia, ausente do pacote, pins iguais, divergência fora de a/b/x/y, conjuntos de identidade diferentes | idem (11 negativos) |
| base invariante | GUID mutável do par congelado em controllers.nxb | idem |
| identidade 0.10.0 | ELF V3 sem NXC6-DOMAIN/banco vivo; marcador /2; fallback genérico em qualquer ELF | idem + `test_gptk_runtime_proof.py` |

# Matriz de regressão do NXRelease 0.3.23

Acréscimos da 0.3.23 (V4-05A, preflight agregado + paridade do piso):

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| preflight read-only | qualquer mutação além do `--out` explícito, em sucesso ou falha | `tests/test_preflight.py` (snapshot) |
| erros independentes | parar no primeiro erro; dependência reprovada virar PASS/SKIP em vez de BLOCKED | `tests/test_preflight.py` |
| receipt | schema divergente do que o nxledger aceita; receipt não vinculado a commit/tree; sobrevivência a um byte de source alterado | `tests/test_preflight.py` |
| caminho público | `public-final` prosseguir sem receipt PASS PUBLIC-FINAL exato (DEV/FAIL/stale/malformado) | `tests/test_preflight.py` + suite |
| paridade do piso | severidades divergentes entre nxabi e nxrelease em qualquer das 7 classes | `tests/test_sdl_floor_parity.py` |

# Matriz de regressão do NXRelease 0.3.22 (histórica)

Acréscimos da 0.3.22 sobre a 0.3.20 (V4-03B, política única de piso SDL):

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| autoridade única | nxabi e nxrelease decidirem por listas diferentes: a closure aceitou Vendor/Product (SDL 2.0.6) via `.syms` paralela | `test_nxrelease.sh` caso 6 + `nxabi/tests/test_sdl_authority.py` (`ConsumerConsistencyTest`: mesmo id, mesmo SHA-256, mesmo mapa) |
| piso por versão | import direto de símbolo SDL2-core nascido acima de 2.0.4 passar por estar na allowlist/closure | `test_nxrelease.sh` casos 6–8 e fixture ELF real `loader-sdl-vendor` |
| primeiro preflight | a violação só aparecer no fim: `validate` (somente leitura) reprova nomeando ELF, símbolo, versão e piso; `stage` reprovado não cria nenhum arquivo | `test_nxrelease.sh` `sdl-floor-preflight` + asserção `stage-sdl-floor` inexistente |
| lista paralela | uma `libSDL2-2.0.so.0.syms` legada reaparecer em `symbol-floors/` | `test_nxrelease.sh` caso 10 (falha fechada no load) |
| autoridade adulterada | tabela ausente, symlink, malformada, duplicata ambígua, versão/id inválidos degradarem para mapa vazio | `test_nxrelease.sh` caso 11 + `StrictParserTest` do nxabi |
| recibo | validate passar sem registrar `sdl_floor`/`sdl_authority`/`sdl_authority_sha256` iguais aos bytes no disco | `test_nxrelease.sh` bloco `validate-receipt` |

# Matriz de regressão do NXRelease 0.3.21

Acréscimos da 0.3.21 sobre a 0.3.20:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| composição corrente | contrato declarativo pina nxinput diferente de 0.9.0 ou nxrelease diferente de 0.3.21 | `declarative-v1.json` + `test_nxrelease.sh` (versões) |
| domínio joydev | projeção ativa sem prova positiva de capabilities, ou seleção por CFW/nome/placa/VID-PID | `nxinput/tests/test_portmaster_domain.c` + matriz C6 |
| duplicata divergente | GUID duplicado divergente rejeita o device em vez de aplicar última-linha-vence com receipt | `nxinput/tests` NXC6 soberano |
| autoridade histórica | metadata 0.3.20 entra no fluxo corrente ou deixa de ser autenticável somente em quarentena read-only | `test_provider_lock.py` |

Acréscimos da 0.3.20 sobre a 0.3.19:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| default do engine | receita sem `validate` é recusada embora o NXExtract 1.3.0 use `data.get("validate", [])` | `test_nxextract_recipe_validate.py`: missing aceita |
| forma explícita | `validate: []` deixa de ser aceita | `test_nxextract_recipe_validate.py`: vazio explícito aceita |
| tipo fechado | objeto ou outro tipo em `validate` ganha a semântica de lista vazia | `test_nxextract_recipe_validate.py`: objeto recusado |
| raízes transacionais | o ajuste torna `extract` ou `commit` opcionais | `test_nxextract_recipe_validate.py`: ambos continuam obrigatórios |
| autoridade histórica | metadata 0.3.19 entra no fluxo corrente ou deixa de ser autenticável somente em quarentena read-only | `test_provider_lock.py` |

Acréscimos da 0.3.19 sobre a 0.3.18:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| anotação Python legítima | `m_VCPassword: Optional[str] = None` é tratado como um literal secreto e bloqueia fonte gerada sem credencial | `test_secret_literal_scan.py`: anotações `.py`/`.pyi` |
| atribuição real | a exceção de anotação deixa `password=abcdefgh` ou `m_VCPassword: Optional[str] = abcdefgh` escapar | `test_secret_literal_scan.py`: atribuição simples e anotada |
| formatos não Python | o ajuste globalmente deixa de recusar `password: abcdefgh` em configuração textual | `test_secret_literal_scan.py`: mapping não Python |
| autoridade histórica | metadata 0.3.18 entra no fluxo corrente ou deixa de ser autenticável somente em quarentena read-only | `test_provider_lock.py` |

Acréscimos da 0.3.18 sobre a 0.3.17:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| composição corrente | nxbootstrap diferente de 0.7.8 ou nxgenerator diferente de 0.3.12 tenta gerar ou publicar candidato corrente | `test_generator_root.py` + `test_public_final.py` |
| mapa nativo estático | default sem `runtime_mapping` promete ser editável ou é recusado apesar de declarar que edições não chegam à engine | `test_generator_root.py` + `nxgenerator/tests/test_gptk_live_contract.py` |
| GPTK vivo preservado | opt-in `nxinput-gptk` recebe o aviso estático, perde o cabeçalho editável ou escapa da prova evento → sink → ACK | `test_generator_root.py` + `test_gptk_runtime_proof.py` |
| autoridade histórica | metadata 0.3.17 não entra no fluxo corrente; somente autoridade externa read-only pode autenticá-la em quarentena | `test_provider_lock.py` |

Acréscimos da 0.3.17 sobre a 0.3.16:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| digest em closure executável | `output_sha256`, `sha_out` ou qualquer outra chave tenta legitimar um 64-hex dentro de fonte/spec | `test_hook_closure_provenance.py`: NXA0050 |
| patch profile não é whitelist de código | digest autenticado em `patch_profiles` reaparece em Python, shell ou dado alcançável | `test_hook_closure_provenance.py`: patch-profile-not-whitelist |
| extensão não decide semântica | os mesmos bytes JSON falham como `.json`, mas passam renomeados para `.blob` | `test_hook_closure_provenance.py`: json-blob-parity |
| closure transitiva | JSON de sufixo desconhecido referencia outro record com digest, ou maiúsculas escapam do scanner | `test_hook_closure_provenance.py`: depth 4 + uppercase |
| binário não vira texto | ELF, NUL, não-UTF-8, conteúdo grande, não-JSON ou escalar entra na closure pelo sniff | `test_hook_closure_provenance.py` + implementação fail-closed limitada |
| caminho positivo | receita autentica selector e fallback genérico; hook/spec consome somente id/ambiente do perfil e não contém digest | `test_hook_closure_provenance.py`: profile-id-positive + fallbacks negativos |

Acréscimos da 0.3.16 sobre a 0.3.15:

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| renderer de private library NXExtract | DSO autenticado `private-library` sob `nxextract/` perde classe `third-party-linux`, modo `0644` ou SHA-256 | `test_nxextract_private_library.py` |
| closure `public-final` | DSO NXExtract autenticado é confundido com extra vivo ou um path não declarado entra na closure | `test_nxextract_private_library.py` + `test_public_final.py` |
| papéis existentes | helper ELF 0644 ou ELF genérico ganha execução/classificação por efeito lateral | gate dirigido nxbootstrap + `test_nxrelease.sh` |
| composição 0.3.16 | nxbootstrap diferente de 0.7.8 ou nxgenerator diferente de 0.3.11 tenta reproduzir a composição histórica | `test_generator_root.py` + `test_generation_store_v2.py` |
| autoridade histórica | metadata 0.3.15 não entra no fluxo corrente; só autoridade externa read-only pode autenticá-la em quarentena | `test_provider_lock.py` |

Acréscimos da 0.3.15 sobre a 0.3.14:

| Fronteira | Falha que deve ser recusada | Prova |
|---|---|---|
| helper NXExtract Linux | ELF AArch64 declarado como `nxextract-helper` volta a ser `payload`, escapa da auditoria Linux ou perde modo `0755` | bloco dirigido de schema 3 em `test_nxrelease.sh` |
| rota genérica fechada | ELF tenta se esconder como `nxextract-spec`, `runtime-data` ou `runtime-hook` | negativos dirigidos de classificação em `test_nxrelease.sh` |
| input nativo | `actions`/`contexts` sem `runtime_mapping` obrigam uma prova GPTK inexistente | positivo `integrations.input = null` em `test_public_final.py` |
| GPTK indevido | receipt de passthrough nativo inventa parser/dispatcher GPTK | negativo `undeclared input integration` em `test_public_final.py` |
| GPTK opt-in preservado | `runtime_mapping = nxinput-gptk` aceita `input = null`, entrega dupla ou símbolos inventados | positivo GPTK e negativos dirigidos em `test_public_final.py` |
| autoridade histórica | 0.3.14 deixa de ser autenticável em quarentena depois do bump | `test_provider_lock.py` |

Acréscimos da 0.3.14 sobre a 0.3.13:

| Fronteira | Falha que deve ser recusada | Prova |
|---|---|---|
| composição 0.3.14 | nxbootstrap diferente de 0.7.7 ou nxgenerator diferente de 0.3.11 tenta reproduzir a composição histórica | `test_generator_root.py` + `test_generation_store_v2.py` |
| autoridade histórica | schema e runtime divergem, ou o candidato 0.3.13 não pode ser autenticado externamente em quarentena | `test_provider_lock.py` |
| promoção de geração | launcher corrente não contém a regra 0.7.7 que exige status 0 junto de health/vídeo | pin byte-exato do launcher + gate dirigido do nxbootstrap |
| Godot fatal no ELF | marker `/2` sem `nxgl_frame_proof_is_fatal` ou `nxgl_frame_proof_consume_fatal`, ou prova que omite a fronteira, tenta passar | `test_gptk_runtime_proof.py` |

Acréscimos da 0.3.13 sobre a 0.3.12:

| Gate | Falha recusada | Prova |
|---|---|---|
| marcadores Godot atômicos | somente um campo, valor desconhecido ou forma extra | `test_gptk_runtime_proof.py` |
| identidade Godot no ELF | prova declara runtime/frame-proof mas os bytes finais não contêm o par | `test_gptk_runtime_proof.py` |
| composição 0.3.13 | nxgenerator diferente de 0.3.10 tenta reproduzir a composição 0.3.13 | `test_generator_root.py` + `test_generation_store_v2.py` |
| autoridade 3 empacotada | runtime nxinput chega à release sem bundle habilitado e pinado | gate do nxgenerator 0.3.10 + auditoria do pacote |
| nome da autoridade 3 | bundle habilitado diverge de `controllers.nxb` e ficaria inalcançável pela declaração NXC6 | `test_v4_optins.py` |

Gates herdados integralmente da 0.3.12:

| Gate | Falha recusada | Prova |
|---|---|---|
| ELF vivo | outro ELF recebe strings marker/schema sem símbolos da API live | `test_gptk_runtime_proof.py` |
| política | contexto nasce menu/gameplay ou integração pode suprimir antes de sink/contexto provados | contrato nxgenerator 0.3.9 + gate dirigido nxinput 0.7.3 |
| evento → sink | load/marker existe, mas não há caso de dispatch/ACK para cada binding e sink | `test_gptk_runtime_proof.py` |
| sink real | receipt nomeia símbolo ausente ou artifact SHA/path stale | `test_gptk_runtime_proof.py` + `candidate-lock-v1.schema.json` |
| segurança | unknown/missing sink suprime ou ACK falho permite replay nativo | normalizador do `input_proof` + gate nxinput |

Acréscimos da 0.3.11 sobre a 0.3.10:

| Gate | Falha recusada | Prova |
|---|---|---|
| composição histórica 0.3.11 | nxbootstrap diferente de 0.7.6 ou nxgenerator diferente de 0.3.9 tenta reproduzir a composição 0.3.11 | `test_generator_root.py` + `test_generation_store_v2.py` |
| autoridade de versão | `VERSION`, `TOOL_VERSION`, schemas de proveniência ou contrato declaram versões diferentes | `test_nxrelease.sh --version` + validação JSON/schema dirigida |
| fronteira de launch SDL do sistema | launcher canônico `sdl_provider=system` não possui o quad legado de `BIN_PRELOAD` e é recusado apesar do guard único `NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD` | `test_generator_root.py` sobre launcher real 0.7.6 |
| restauração de `SDL_DYNAMIC_API` | valor já pertencente ao firmware/adapter, propagado por `NXBOOTSTRAP_SYSTEM_SDL_*`, é confundido com SDL privada; qualquer outro redirect continua fechado | `test_provider_lock.py` (positivo canônico + negativo local) |

A 0.3.10 preserva o gate declarativo de `controls.runtime_mapping =
nxinput-gptk`: marcador no ELF e default GPTK parseável são obrigatórios; uma
declaração sem runtime ou um arquivo que se anuncie editável sem opt-in falham
na renderização canônica.

Acréscimos da 0.3.9 sobre a 0.3.8:

| Gate | Falha recusada | Prova |
|---|---|---|
| seed generation-v2 obrigatório | manifesto e `GENERATION.json` omitem juntos o seed enquanto mantêm a geração oculta | `test_generator_root.py`: negativo coordenado no core |
| identidade do header | `generation` do seed difere da identidade do store; magic não canônico também continua recusado | `test_generator_root.py` (geração estrangeira) + `test_v4_repro.py` (parser) |
| igualdade seed/store | membro autenticado no seed diverge em bytes/SHA do mesmo membro no store; digest, trailing bytes, offset e colisão de caixa continuam fechados | `test_generator_root.py` (divergência coordenada) + `test_v4_repro.py` (estrutura NXB) |
| fechamento source/stage/ZIP | a mesma validação semântica integra o caminho comum auditado em source, stage e ZIP reaberto | `test_generator_root.py` (validate/stage/verify-stage) + `test_public_final.py` (archive reaberto) |
| reconstrução sem dotdir | apagar toda `.nxruntime` não recompõe a geração pelo seed autenticado, não escreve `commit` por último ou alcança NXExtract/jogo com closure divergente | `nxbootstrap/tests/test-v4-bundle.sh` |
| história preservada | artefato 0.3.8 ou anterior é reinterpretado pela política 0.3.9 ou entra em publicação corrente fora da quarentena read-only | `test_public_final.py` + `test_provider_lock.py` |

Acréscimos da 0.3.8 sobre a 0.3.7:

| Gate | Falha recusada | Prova |
|---|---|---|
| composição canônica | nxbootstrap diferente de 0.7.4 ou nxgenerator diferente de 0.3.7 falha fechado | `test_generator_root.py` + `test_public_final.py` |
| SDL pública pelo sistema | SDL1/2 privada por basename, SONAME, exports dynamic/hidden/static, add-ons gfx/gpu/sound, provider package, `SDL_DYNAMIC_API`, LD path específico ou `dlopen` local | `test_provider_lock.py` via `test_nxrelease.sh` |
| exceção SDL3 fechada | texto fake, não-ELF, ABI/SONAME/mode/path/dependency/licença/proveniência divergente, SHA errado, opt-in vazio ou `sdl_provider=system` | `test_provider_lock.py`: DSO AArch64 real positiva + negativos |
| lock externo do candidato | lock dentro do source, gravável, symlink, hardlink, parent inseguro, TOCTOU, path/SHA divergente ou executável trocado | `test_provider_lock.py`: fd `O_NOFOLLOW` + fstat/inode + source/stage/ZIP |
| prova de vídeo obrigatória | `video_proof=required` sem os dois símbolos, sem schema/receipt/env, receipt ausente, BLACK, hash inventado, port ou geração stale | `test_provider_lock.py` via `test_nxrelease.sh` |
| política não autoatestada | `metadata.tool.version` antigo tenta desligar lock/SDL; `public-final` usa só a projeção do lock embutida | `test_provider_lock.py` + `test_public_final.py`: autoridade externa original obrigatória |
| quarentena histórica/publicação corrente | 0.2.40–0.3.19 sem autoridade externa/SHA exato, fora de path quarantine, `physical_support_proven=false` ou tentativa de `public-final` legado | `test_provider_lock.py` + `test_public_final.py`; `publication_eligible=false` |
| uma compilação/pacote | build-B retorna; `build`/`bundle` repetem stage/package sem opt-in; provenance alega double-build | `test_public_final.py` + `test_v4_repro.py` |

As seções antigas abaixo registram o comportamento das versões em que foram
criadas. Onde mencionam build A/B ou promoção histórica, as políticas 0.3.8 e
0.3.9 acima as substituem para qualquer operação corrente/publicável.

Acréscimos da 0.3.7 sobre a 0.3.6:

| Gate | Falha recusada | Prova |
|---|---|---|
| composição canônica | nxbootstrap diferente de 0.7.3 ou nxgenerator diferente de 0.3.6 falha fechado | `test_generator_root.py` + `test_public_final.py` |
| reprodução histórica | candidato autenticado 0.3.6 continua reproduzível; candidato novo declara 0.3.7 | `test_public_final.py` |

Acréscimos da 0.3.6 sobre a 0.3.5:

| Gate | Falha recusada | Prova |
|---|---|---|
| `runtime-data` live/store | assembly 0644 entra como payload não ELF, byte-idêntico na árvore ativa e na geração imutável | `test_generation_store_v2.py` + `test_generator_root.py` |
| composição canônica | nxbootstrap diferente de 0.7.2 ou nxgenerator diferente de 0.3.5 falha fechado | `test_generator_root.py` + `test_public_final.py` |
| reprodução histórica | candidato autenticado 0.3.5 continua reproduzível; candidato novo declara 0.3.6 | `test_public_final.py` |

Acréscimos da 0.3.5 sobre a 0.3.4:

| Gate | Falha recusada | Prova |
|---|---|---|
| versão .NET estruturada | `assemblyVersion`/`fileVersion` de quatro partes em `.deps.json` passam | `test_v4_repro.py` |
| privacidade preservada | IP em outro campo do mesmo `.deps.json` e versão semelhante fora dessa extensão falham | `test_v4_repro.py` |
| reprodução histórica | candidato autenticado 0.3.4 continua reproduzível; candidato novo declara 0.3.5 | `test_public_final.py` |

Acréscimos da 0.3.4 sobre a 0.3.3:

| Gate | Falha recusada | Prova |
|---|---|---|
| runtime vazio PortMaster | metadado v4 sem `attr.runtime` passa; `attr.runtime: []` falha com forma canônica explícita | `test_nxrelease.sh` + ciclo HarbourMaster real |
| runtime não vazio | null, string, item inválido ou duplicata continuam falhando; lista não vazia permanece válida | `test_nxrelease.sh` |
| composição canônica | candidato embutido diferente de nxgenerator 0.3.4 falha | `test_generator_root.py` + `test_public_final.py` |
| reprodução histórica | candidato autenticado 0.3.3 continua reproduzível; candidato novo declara 0.3.4 | `test_public_final.py` |

Acréscimos da 0.3.3 sobre a 0.3.2:

| Gate | Falha recusada | Prova |
|---|---|---|
| privacidade por membro do seed | índice ASCII não faz o ELF interno parecer texto; multicast SSDP binário passa, mas IPv4 em texto e path privado em binário falham | `test_v4_repro.py` |
| autenticação da fronteira | membro alterado, payload truncado/trailing, offset descontínuo, duplicata/case-collision e header fora da forma canônica falham fechados | `test_v4_repro.py` + parser `NXBUNDLE1` |
| reprodução histórica | candidato autenticado 0.3.2 continua reproduzível; candidato novo declara 0.3.3 | `test_public_final.py` |

Acréscimos da 0.2.43 sobre a 0.2.42:

| Gate | Falha recusada | Prova |
|---|---|---|
| Recibo de content rect | `display.remap_input` sem `display_proofs`, com veredito escrito à mão, com política/tamanho divergentes, com rect vazio, sem device ou com device duplicado falham fechado | `test_v4_optins.py` |
| reprodutibilidade obrigatória | `build`/`bundle` sempre refazem stage+ZIP e comparam; nenhuma flag desliga | `test_v4_repro.py` |
| comparação de ELF | ELF divergente, inventário diferente ou ZIP diferente falham fechado e o ELF é nomeado | `test_v4_repro.py` |
| `BUILD-PROVENANCE.json` | escrito `0644`, no-replace, sanitizado, com toolchain e hash de cada ELF | `test_v4_repro.py` |
| opt-ins V4 declarados | forma de `display`/`egl_binding`/`input_sdl3_portmaster`; contrato antigo sem os blocos continua válido | `test_v4_optins.py` |
| `egl_binding` × `DT_NEEDED libEGL` | binding ligado com ELF linkando EGL falha fechado | `test_v4_optins.py` |
| composição canônica | launcher diferente de nxbootstrap 0.6.37 ou receipt diferente de nxgenerator 0.2.20 | `test_nxrelease.sh` + `test_generation_store_v2.py` + `test_generator_root.py` |
| compatibilidade de candidato congelado | candidato autenticado 0.2.42 recusado ou reescrito como 0.2.43 | `test_public_final.py`: 0.2.40/0.2.41/0.2.42/0.2.43 positivos |
| identidade do promotor | `BUILD-PROVENANCE.json` apresenta versão diferente do executor corrente | schema `build-provenance-v1`: 0.2.43 |

Acréscimos preservados da 0.2.42 sobre a 0.2.41:

| Gate | Falha recusada | Prova |
|---|---|---|
| reprodução de candidato congelado | build A/B reescreve metadata/SBOM 0.2.40 com a versão corrente e altera o ZIP físico já testado | `test_public_final.py`: seletor → `_public_final_build` → `stage_release` real → metadata/SBOM 0.2.40 |
| autoridade da versão | versão escolhida por argumento/manifesto, ausente ou anterior à allowlist schema-compatível | `test_public_final.py`: 0.2.40/0.2.41/0.2.42 positivos e 0.2.39 negativo |
| identidade do promotor | `BUILD-PROVENANCE.json` apresenta a versão antiga do artefato no lugar do NXRelease que executou o gate | schema `build-provenance-v1`: 0.2.42; igualdade tested/build-A/build-B permanece obrigatória |

Acréscimos da 0.2.41 sobre a 0.2.40:

| Gate | Falha recusada | Prova |
|---|---|---|
| update A/B real | geração anterior autenticada confundida com arquivo stale e recusada pelo overlay HarbourMaster | `test_harbourmaster_cycle.py`: v1→v2 e v2→v2, `retained_generations=1` |
| migração histórica | primeiro opt-in store somente a partir de ZIP realmente simples; v1 de 32 hex/mixed-ABI com `execution_roles`; ordem por componentes de path até nxgenerator 0.2.18 e POSIX textual em 0.2.19+ | `test_harbourmaster_cycle.py`: first-adoption + legacy-v1 |
| exceção fail-closed | receipt/store órfão inclusive em instalação limpa, arquivo solto sob path semelhante ao store, root extra, closure parcial, commit/manifest/hash divergente, ordem ou modo adulterado, token de launcher duplicado e colisão do mesmo id | `test_harbourmaster_cycle.py` + ciclo real `verify --previous-archive` |

Acréscimos da 0.2.40 sobre a 0.2.39:

| Gate | Falha recusada | Prova |
|---|---|---|
| identidade do gerador | candidato diferente do nxgenerator 0.2.19 exato, inclusive 0.2.18 | `test_nxrelease.sh` + `test_generator_root.py` contra a branch 0.2.19 |
| biblioteca privada live/store | `private-library` ELF autenticada como `0644` reescrita para `0755` pelo scanner `lib/*.so*`, ou qualquer drift posterior `0644→0755` no store | `test_generator_root.py`: quatro pares ELF reais + negativo dedicado |
| igualdade imutável | path lógico, modo, SHA-256 ou metadados ELF diferentes entre membro live e store | `test_generator_root.py` + `test_public_final.py` |

Acréscimos da 0.2.39 sobre a 0.2.38:

| Gate | Falha recusada | Prova |
|---|---|---|
| identidade do gerador | candidato diferente de nxgenerator 0.2.18 ou receipt autoral emitido pela 0.2.17 | `test_nxrelease.sh` + `test_generator_root.py` |
| payload autoral fechado | path/kind/modo/SHA divergente, arquivo omitido ou overlay posterior fora de `GENERATION.json` | `test_generator_root.py` + teste dirigido do nxgenerator |
| recibo e closure exatos | hash do projeto divergente, artefato acrescentado/removido depois da geração ou SHA-256 autoral não canônico | `test_generator_root.py` + `test_public_final.py` |
| ELF live/store | executável, NXSplash ou NXExtract UI do store tratado como dado; classe/metadados ausentes; path lógico, modo ou SHA divergente | `test_generator_root.py` + `test_public_final.py` |
| refresh schema 3 | hash stale de `package_payload`, symlink/hardlink ou modo diferente da declaração | `test_refresh_pins_schema3.py` |
| tuning GPTK | cursor/câmera específicos perdidos na geração package-shaped | `test_generator_root.py` + teste dirigido do nxgenerator |

Acréscimos da 0.2.38 sobre a 0.2.37:

| Gate | Falha recusada | Prova |
|---|---|---|
| identidade do gerador | candidato diferente de nxgenerator 0.2.17; tentativa de usar a 0.2.16 com a composição NXExtract incompleta | `test_nxrelease.sh` + `test_generator_root.py` + `test_generation_runtime.py` |
| schema 3 + NXExtract real | colisão ao materializar novamente `nxextract/`; membro pinado diferente do NXExtract canônico | `test_generation_runtime.py` e candidatos reais BB1/BB2/OTR antes de ZIP |
| bateria canônica | teste dirigido novo existe no repositório, mas não é classificado nem executado por `run-safe-gates.sh` | `test_infrastructure.py` + `test-matrix-v1.json` |

Acréscimos da 0.2.37 sobre a 0.2.36:

| Gate | Falha recusada | Prova |
|---|---|---|
| promoção run-bound | lifecycle declarado como completo sem `NXU0006`, ou com `NXU0005`/`NXU0007`, run diferente ou generation diferente | `test_public_final.py`: positivo exato + cinco negativos de health |
| build externo a partir de fonte limpa | manifesto completo carregado antes do ELF ignorado existir; saída externa ausente/extra/symlink; hash divergente; override de kind não-project; input não-project ausente | `test_public_final.py`: preflight estreito + `load_manifest` real e negativos de fronteira |
| worktree realmente limpa | `git status` vazio enquanto ELF/cache ignorado pelo `.gitignore` permanece na árvore | `test_public_final.py`: fixture `ignored-elf` recusada por `git ls-files --others --ignored` |
| closure transitiva de hook | entrypoint chama subprocess/import que abre spec JSON com SHA de entrada não declarado; fallback `none` disfarçado de texto | `test_nxrelease.sh`: profundidade 3, negativo NXA0050, positivo por patch-profile preflight e fallback rejeitante recusado |
| geração v2 fechada | live/store híbrido, controle stale, membro extra/ausente, role/path/mode/hash divergente ou closure NXExtract incompleta | `test_generation_store_v2.py` + `test_public_final.py` + suíte `nxbootstrap 0.6.37` |
| candidato pós-build real | ELF recompilado depois do pin, launcher solto, root extra ou `GENERATION.json` fora do pacote | `test_generator_root.py` + `test_refresh_pins_schema3.py` |
| selamento sem fuga | `runtime_root`, diretório intermediário, membro ou `nxproject.json` symlink; hardlink final; source schema 3 dependente de manifesto legado | `test_refresh_pins_schema3.py`: positivo sem `nxrelease.json`, oito negativos e smoke legado |
| verificação independente | build helper contém marcador reconhecível mas produz geração stale | `test_nxrelease.sh`: `nx-ship-port` aceita/rejeita os bytes pelo `nxrelease verify` normal |

Acréscimos da 0.2.36 sobre a 0.2.35:

| Gate | Falha recusada | Prova |
|---|---|---|
| round-trip do pin de geração | `verify_stage` perde o SHA-256 autenticado ao reconstruir records e recusa GPTK/settings válidos como não pinados | `test_nxrelease.sh`: fixture com `generation_id` atravessa build, verify-stage, rebuild idêntico e bundle |
| pin fail-closed | `defaults/NEXTOSCONTROLLERS.gptk` sem SHA-256 ou com SHA-256 divergente | negativos `generation-defaults-pin-missing` e `generation-defaults-pin-wrong` antes de criar stage |

Acréscimos preservados da 0.2.35:

- o receipt GPTK precisa nomear o loader, parser, dispatcher e source guard
  reais do nxinput 0.5.1; aliases inventados falham;
- todo áudio declarado precisa provar receipt e liveness canônicos;
- `recovery_state=recovered` só passa com o ciclo limitado e receipt do
  nxaudio 0.3.1;
- o launcher canônico corrente é nxbootstrap 0.6.37.

Acréscimos preservados da 0.2.34:

| Gate | Falha recusada | Prova |
|---|---|---|
| separação dev/final | scaffold ou package estrutural confundido com release; `unimplemented_nonrelease`; `release_ready=false`; lifecycle vazio | `test_public_final.py` + label `DEV/PACKAGE PASS` |
| coerência da promoção | divergência nxproject ↔ nxport ↔ adapter ↔ claims ↔ generation runtime; geração ausente ou identidade externa não explicitada | positivos/negativos de geração em `test_public_final.py` |
| receipts exatos | receipt de outro ZIP, ELF, build-id, generation, commit ou device; ausência de cobertura declarada | `test_public_final.py` + schema `public-final-receipt-v1` |
| integração runtime | graphics declarado sem contrato/probe/símbolos; GPTK sem parser/dispatcher/sink; entrega dupla; callback de áudio morto; hotkey não independente | receipts tipados e auditoria de símbolos do ELF final |
| imagem real | frame proof `BLACK`, inconclusivo, sem amostras ou sem pixel não preto | negativo BLACK + `GRAPHICS-EVIDENCE` ligado ao run/generation/commit |
| reprodutibilidade final (histórico, substituído em 0.3.8) | árvore suja; saída ausente/extra/symlink; ZIP, inventário ou ELF divergentes | cobertura original de `test_public_final.py`; política corrente usa candidato testado + um rebuild |
| migração honesta (histórico, substituído em 0.3.8) | ZIP root-less promovido implicitamente ou release histórica apresentada como public-final | política corrente recusa external-generation e conserva histórico somente em quarentena |

Acréscimos da 0.2.33:

| Gate | Falha recusada | Prova |
|---|---|---|
| versões canônicas correntes | TOOL/VERSION divergentes; nxbootstrap diferente de 0.6.37; NXExtract diferente de 1.2.21; nxgenerator diferente de 0.2.20 | `test_nxrelease.sh` + `test_nxextract_engines.py` |
| GPTK runtime real | aliases inexistentes; falta de loader, parser, dispatcher, source guard ou proteção da origem primária | `test_public_final.py` |
| áudio runtime real | callback sem receipt/liveness; recuperação alegada sem ciclo limitado/receipt | `test_public_final.py` |
| identidade do engine | engine/runner 1.2.21 trocados ou 1.2.20 apresentado com hashes da versão nova | registro imutável + build/verify/reabertura |

Herdada da 0.2.32:

| Gate | Falha recusada | Prova |
|---|---|---|
| hooks NXExtract próprios do port | receita referencia `{game_dir}/nxextract/<hook>`, mas o renderer omite o arquivo; modo não executável; inclusão de symlink, `.pyc` ou duplicata canônica pelo caminho adicional | `test_nxrelease.sh` (`nxrelease port hook packaging regression passed`) |

Herdada da 0.2.31 (V3):

| Gate | Falha recusada | Prova |
|---|---|---|
| apkcompat canônico | sha256/crc32 de container em qualquer quantidade; size exato; whitelist legada de 2 SHAs | `test_nxrelease.sh` (legacy_identity_negative, exact-apk) + fixtures compartilhadas |
| defesa estática de hook | 64-hex, versão pontuada literal, tabela de tamanhos, offset absoluto em argv/env/closure transitiva; hook V3 sem contrato | `validate_recipe_hooks_static` + suíte |
| gamedata três fronteiras | marcador ausente/kind/modo/hash stale/nível extra; INSTALLATION sem o caminho; search_dirs divergente; ZIP extraído sem o diretório físico | `test_nxrelease.sh` (gamedata negatives) |
| defaults de controles | geração com generation_id sem defaults/NEXTOSCONTROLLERS.gptk; .gptk do dono dentro do ZIP | `validate_generation_receipt` + audit |

Herdada da 0.2.26:

| Gate | Falha recusada | Prova |
|---|---|---|
| PortMaster v4 real | `runtime` ausente/tipo errado/duplicado, title/arch/GLIBC divergente, `items` inválido ou metadata v3 | 10 mutações + parser real `port_info_load`/`HarbourMaster.load_ports` |
| Lifecycle PortMaster | ZIP apenas parseável, update sem overlay real, órfão, uninstall excessivo ou reinstall divergente | `harbourmaster-cycle.py` offline; `verify --previous-archive` |
| JSON estrito | BOM, chave duplicada, `NaN`, vírgula final ou truncamento no manifesto/metadado reaberto | 5 fixtures de manifesto + 3 ZIPs recompostos |
| INSTALLATION.md | ausente, sem pin/modo estável ou sem uma das duas línguas | 4 contratos negativos + reabertura do ZIP |
| NXExtract UI multiarch | ausente, relabel, hash/mode/ABI/GLIBC divergente ou UI ARMv7 em port AArch64 | `test_nxrelease.sh` |
| Manifesto da UI | path/tamanho/SHA/fonte/classe/machine divergente da linha canônica 1.2.9 | `test_nxrelease.sh` + `readelf` |
| Kit NXExtract | engine 1.2.13/core/runner/helper auto-pinados, sem resultado terminal ou sem atestação | `test_nxrelease.sh` + launcher canônico 0.6.27 |
| Mixed-ABI opt-in | ABI/classe/machine/interpreter/closure divergente entre UI AArch64 e splash/jogo ARMHF, ou receipt diferente do nxport | `test_nxrelease.sh` com os ELFs canônicos reais |
| APK flexível | package ausente, tamanho exato, uma única identidade SHA/CRC ou container sem âncora forte | `test_nxrelease.sh`; receitas reais Angry Birds, Retro Highway e ScourgeBringer |
| NXSplash sempre | ausência, troca, skip ou ordem errada | `test_nxrelease.sh` |
| Firmware | 8 perfis separados e 14 casos, sem promover fixture a hardware | `firmware-profiles-v2.json` + runner/receipt sintéticos (`hardware_ran=false`) |
| Shell portátil | `stat` direto/indireto em qualquer shell allowlisted, helper extensionless, `nxbootstrap.sh` legado ou retorno mudo | PATH sem `stat`, testes prepare/runner/runtime/helper e auditor do ZIP real |
| Vídeo físico isolado | renderer sem prontidão ou claim visual maior que a prova | NextOS/Mali-450: `visible=sdl`/mali e captura gráfica válida; ArkOS-class/KMSDRM: somente `visible=sdl`, log KMSDRM e recovery, sem captura visual válida; nenhum dos dois implica instalação/jogo completo |
| Integridade | ZIP/SBOM/MANIFEST/ELFs/dependências divergentes | build + verify + reabertura |
| Pins opt-in | nxbootstrap diferente de 0.6.27, engine NXExtract diferente de 1.2.13, UI diferente de 1.2.9 ou nxsplash diferente de 0.1.2 | `test_nxrelease.sh` |
| Identidade visual | alteração da UI gráfica NXExtract ou da tela NEXTOS/RETRO ELITE | golden visual dos componentes + aceitação física |

ROCKNIX/Panfrost continua `physical-runtime-ui-unverified`: o histórico TASM2
extraiu mais de 1 GiB, mas a UI ficou preta. AmberELEC tem somente evidência de
subsistema launcher antigo; Knulli/Batocera é parcial; TrimUI não possui receipt
publicado. MIYOO_EX e RetroDECK são apenas raízes candidatas de descoberta, sem
fixture/runtime, portanto `unsupported/unverified`.

O gate host não inventa prova física. O mesmo ZIP final ainda precisa mostrar a
UI gráfica NXExtract durante instalação, a NXSplash NEXTOS/RETRO ELITE em toda
abertura e a imagem do jogo no hardware declarado antes da publicação. O ELF
AArch64 NXSplash 0.1.2 de SHA-256
`d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97`
continua `physical-unverified` até o mesmo ZIP Hitman GO ser aceito nos dois
aparelhos autorizados.

## 0.3.26 — classe de evidência ON_DEVICE_AUTOMATED_INPUT_PROOF

| Gate | Regressão rejeitada | Evidência |
|---|---|---|
| evidence_class | `HOST_FIXTURE` ou classe desconhecida aceita no lock; classe perdida na normalização | `tests/test_on_device_input_proof_class.py` |
| contrato com controls.proof | port com prova declarada aceito sem a classe on-device | `tests/test_on_device_input_proof_class.py` |
| nx-input-proof-lock | receipt falho, de outro ELF, de outra classe ou com binding não observado virando lock | `tests/test_on_device_input_proof_class.py` |

## V4-GRAPHICS-04 (0.3.1)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Boundary declarado, receipt pós-present | `phase=post-first-present` + `first_present=1` + `pre_drawable` + commit consistente promovem | `tests/test_nxrelease.sh` |
| Boundary ausente | Gate anterior byte a byte; receipt clássico sem campos novos continua aceito | `tests/test_nxrelease.sh` |
| Fail-closed | Valor desconhecido de boundary, receipt pré-present, `first_present` ausente, `pre_drawable` ausente, commit ausente ou divergente, `drawable=1x1` e receipt reutilizado (one-shot) nunca promovem | `tests/test_nxrelease.sh` |
