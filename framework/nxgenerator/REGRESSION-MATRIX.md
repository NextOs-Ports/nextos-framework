# Matriz de regressão do nxgenerator 0.4.5

## Composição V5 com nxbootstrap 0.8.4

| Caso | Prova |
|---|---|
| receipt fixa nxbootstrap 0.8.4 e os SHA-256 reais do gerador/template | `tests/test_video_provider_contract.py` + `tests/test_generation_runtime.py` |
| campos provider/vídeo ausentes continuam sem serialização e bytes alheios permanecem iguais | idem |
| launcher `sdl_provider=system` transporta o resolver canônico e a sanitização estreita da SDL herdada obsoleta com `$LIB`, sem o nxgenerator reimplementá-los | idem + suíte isolada do nxbootstrap |

## 0.4.5 — `auto-stretch` declarativo

| Caso | Prova |
|---|---|
| `auto_algorithm=stretch` com `auto` declarado é aceito e preservado | `tests/test_v5_owner_video.py` |
| heurística por aparelho/modelo continua fora do enum fechado | idem |
| `ratio-threshold`, `epsilon` e ausência de vídeo mantêm a semântica anterior | idem |

## 0.4.4 — canal do gatilho por todos os contextos

| Caso | Prova |
|---|---|
| R2 null na base e botão no override → `mode = digital` + `trigger.right.digital = action:…` | `tests/test_v5_gptk4_trigger_override.py` |
| botão e analógico no mesmo gatilho em contextos distintos → `ProjectError` | idem |


## 0.3.17 — START isolado antes dos negativos

| Caso | Prova |
|---|---|
| START ligado a ação: entrega medida antes do `chord_cross`, press/release uma vez, segundo START desfaz o pause | `tests/test_input_proof_roteiro.py` (ordem START < unplug < negativos) |


## 0.3.16 — roteiro automático da prova de controles (ON_DEVICE_AUTOMATED_INPUT_PROOF)

| Gate | Regressão rejeitada | Evidência |
|---|---|---|
| controls.proof | membro desconhecido, navegação faltando para um contexto declarado, quit_guard/owner_remap fora do contrato, clones fora de 1..3 | `tests/test_input_proof_roteiro.py` |
| roteiro gerado | controle ligado sem entrega/press-release, `null` sem supressão, `native` com entrega, stick sem volta ao neutro, negativos (L1+R1, L2+R2, SELECT, START, cross-pad) ausentes, sem SELECT+START final | `tests/test_input_proof_roteiro.py` |
| owner remap gerado | arquivo do dono igual ao default, ou roteiro sem supressão do `null` e entrega no destino | `tests/test_input_proof_roteiro.py` |
| adapter intocado | `controls.proof` vazando para o adapter-contract promovido ou para o pacote | `tests/test_input_proof_roteiro.py` |

## Pré-verificação da autoridade (0.3.14, V4-04C)

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| identidade pinada | engine adulterado (mesmo com a mesma string de versão) executa qualquer byte antes da recusa | `test_recipe_authority.py`: sentinela nunca criada |
| caminho canônico | candidato/source root/env escolhe o módulo, ou componente symlink/escape/não-regular/FIFO/oversize é aceito | `test_recipe_authority.py`: casos de caminho |
| versão por AST | expressão dinâmica, duplicata, ausência ou divergência passa | `test_recipe_authority.py`: casos de versão |
| snapshot único | troca entre verificação e uso executa bytes novos | `test_recipe_authority.py`: swap pós-leitura |
| cache | falha deixa módulo parcial em `_NXEXTRACT_AUTHORITY` | `test_recipe_authority.py`: cache vazio |
| sanitização | mensagem revela pathname/traceback | `test_recipe_authority.py`: varredura de vazamento |

## Receita = autoridade NXExtract 1.3.0 (0.3.13, V4-04B)

Inventário antes/depois — decisões estruturais do `extractor.json`:

| Decisão | 0.3.12 (validador isolado) | 0.3.13 (autoridade única) | Prova |
|---|---|---|---|
| root não-objeto | recusava | recusa (autoridade) | corpus `invalid-root-array` |
| `extract`/`commit` ausentes ou tipo errado | **ACEITAVA** (divergia do NXExtract) | recusa | corpus + controle negativo end-to-end contra o 0.3.12 |
| `validate` ausente | aceitava por omissão | aceita (= `[]`, semântica 1.3.0) | corpus `valid-validate-absent` |
| `validate: []` | aceitava por omissão | aceita | corpus `valid-validate-empty` |
| `validate` objeto/string/null/número/bool | **ACEITAVA** | recusa | corpus `invalid-validate-*` |
| chave duplicada / BOM / UTF-8 inválido | **ACEITAVA** dup; BOM/UTF-8 recusava por parse | recusa (loader estrito da autoridade) | corpus |
| NaN em campo tipado (`space.safety_bytes`) | aceitava | recusa (tipo int da autoridade) | corpus `invalid-nan-typed-field` |
| NaN em campo desconhecido | aceitava | aceita — decisão explícita e documentada da autoridade 1.3.0 (campos não tipados); a fronteira JSON estrita do nxrelease permanece dona do resto | corpus `valid-nan-unknown-field` |
| unknown fields | tolerava | tolera (autoridade tolera; receita histórica não estreitada) | corpus `valid-unknown-fields` |
| oversize >1 MiB / nesting abusivo | aceitava/traceback | recusa (`MAX_RECIPE_BYTES`; `RecursionError`→`ProjectError`) | casos gerados no gate |
| bool aceito como int | n/a | `isinstance(x, bool)` recusado nos campos int da autoridade | `Recipe._validate` |
| política APK-variant (apkcompat 1.1.0) | do generator | **inalterada** — aditiva, já resolvia a identidade flexível de container (PRE-04 parcial concluído lá) | `validate_apk_variant_policy` sobre a receita validada |
| `gamedata` primeiro em `search_dirs` | do generator | **inalterada** — aditiva; a receita mínima canônica a satisfaz | gate `test_recipe_authority` |
| versão da autoridade | não conferida | engine `NXEXTRACT_VERSION` **e** arquivo `VERSION` = 1.3.0, fail-closed | gate de drift com engine falso |
| origem do código | — | somente `NXEXTRACT_ENGINE` do framework; nunca source_root | asserção de path no gate |

## Mapa estático versus GPTK vivo (0.3.12)

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `controls.actions`/`contexts` sem `runtime_mapping` | default identifica mapa estático e avisa que edições não chegam à engine; nenhuma promessa "Este arquivo é SEU"/"This file is YOURS" | `tests/test_gptk_live_contract.py` + `tests/test_c4_gptk_bytes.py` |
| `runtime_mapping = nxinput-gptk` | cabeçalho editável anterior permanece literal e o aviso estático não aparece | `tests/test_gptk_live_contract.py` |
| schema 1/2 e bindings | somente o cabeçalho muda no caminho estático; magic, ações, `native` e `null` permanecem canônicos | `tests/test_c4_gptk_bytes.py` |

## Composição nxbootstrap 0.7.8 (0.3.12)

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| geração corrente | receipt identifica nxgenerator 0.3.12 e fixa os sources byte-exatos do nxbootstrap 0.7.8 | `tests/test_video_provider_contract.py` + `tests/test_generation_runtime.py` |
| promotion com autoridade 3 | `input_controller_profiles` no contrato owner é byte-sematicamente igual ao pin normalizado e permanece no adapter renderizado | `tests/test_gptk_live_contract.py` |
| promotion omite, inventa ou troca o pin | geração falha antes de publicar; promoção nunca encobre a perda de `controllers.nxb` | `tests/test_gptk_live_contract.py` |

## Autoridade 3 obrigatória (0.3.10)

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `runtime_mapping = nxinput-gptk` sem `controller_profiles` | falha antes de gerar qualquer saída | `tests/test_gptk_live_contract.py` |
| bundle ausente ou desabilitado | falha fechada; a ordem soberana não pode perder a autoridade empacotada | `tests/test_gptk_live_contract.py` |
| bundle habilitado com nome diferente de `controllers.nxb` | falha antes da geração; o pacote não pode divergir do caminho declarado pelo runtime NXC6 | `tests/test_gptk_live_contract.py` |
| bundle habilitado e pinado | contrato aceito sem relaxar a política GPTK 0.3.9 | `tests/test_gptk_live_contract.py` |
| port sem runtime nxinput | nenhum campo ou comportamento de runtime é inventado; o default se identifica honestamente como mapa estático e a identidade/pins registram 0.3.12 | `tests/test_gptk_live_contract.py` |

## Provider SDL e prova de vídeo (0.3.7)

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| campos ausentes numa geração 0.3.7 | `sdl_provider` e `video_proof` não aparecem no `nxport.json`; comparada à mesma composição com opt-in, adapter, controles, metadata, documentação, licença e payload NXExtract preservam bytes | `tests/test_video_provider_contract.py` |
| `sdl_provider: system` | round-trip literal no projeto/manifesto; launcher ativa resolução de sistema sem forçar `SDL_VIDEODRIVER` | `tests/test_video_provider_contract.py` |
| `video_proof: required` | round-trip literal; launcher exige receipt de vídeo e o pin de fonte identifica exatamente a composição corrente nxbootstrap 0.7.8 | `tests/test_video_provider_contract.py` |
| os dois opt-ins | identidade de geração diverge da árvore sem declaração; nenhum artefato alheio aos opt-ins muda | `tests/test_video_provider_contract.py` |
| provider/prova desconhecido, vazio, nulo ou booleano | geração recusada antes de publicar saída | `tests/test_video_provider_contract.py` |
| gate padrão de generation-runtime | importa e executa o gate provider/vídeo, valida seu resultado exato, captura stdout/stderr e preserva a saída histórica do gate chamador | `tests/test_generation_runtime.py` |

O gate é exclusivamente host e não declara imagem física. O produtor de
receipt deve medir o framebuffer real antes de `present`; screenshot DRM/GBM,
PID e áudio não são autoridades de imagem.

A 0.3.6 compõe 0.3.5 + 0.3.3; as duas matrizes abaixo valem integralmente.

Acréscimos da 0.3.5 sobre a 0.3.4:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `runtime-data` 0644 pinado | assembly gerenciado aparece byte-idêntico na raiz ativa e no store V2, integra o seed e entra no receipt 0.3.5 | `test_generation_runtime.py` |

Acréscimos da 0.3.4 sobre a composição integrada 0.3.2:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `portmaster.runtime: []` válido | `port.json` v4 omite `attr.runtime`; o ciclo real do HarbourMaster atual instala/descobre/remove/reinstala | `test_nxgenerator.py` + `test_harbourmaster_cycle.py` |
| projeto sem/null/escalar/duplicado | continua recusado antes da geração; a omissão vale somente para a saída de uma lista vazia validada | `test_nxgenerator.py` |
| lista de runtime não vazia | lista preservada literalmente no `port.json`; nenhuma alegação legada nova é criada | `test_nxgenerator.py` |
| manifesto legado v1 sem runtime | saída v4 usa a mesma omissão compatível; mixed ABI legado continua recusado | `test_nxgenerator.py` |
| recibo | fixa contrato PortMaster v3, schema v2 e commits dos parsers atual e legado | `test_nxgenerator.py` |

## V4-NXGENERATOR-NEXTOS-CATALOG-01 (0.3.3)

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| Objeto no formato literal de Ronax | saída contém exatamente `id`, `name`, `description`, `build`, `links`, `images`, `installation`, sem campo injetado | `tests/test_nextos_catalog.py` |
| Mesma ficha + mesmo nxproject, duas saídas novas | bytes UTF-8 idênticos, newline final e modo `0644` | `tests/test_nextos_catalog.py` |
| Destino já existente | exportação recusa sem alterar o hash dos bytes existentes | `tests/test_nextos_catalog.py` |
| Binding editorial/técnico | `id` é `<nxport.id>-nextos`, `name` acompanha o título, repo público acompanha `id`, URL é `releases/latest/download`, ZIP acompanha `nxport.id` e instalação acompanha `gamedata/` | `tests/test_nextos_catalog.py` |
| Campo ausente/extra, tipo/versão inválido ou título divergente | falha fechada antes da publicação | `tests/test_nextos_catalog.py` |
| HTTP, credenciais, localhost/IP, release de outro repo ou asset divergente | falha fechada antes da publicação | `tests/test_nextos_catalog.py` |
| Path traversal, imagem fora de `port-json/<id>/`, lista vazia/duplicada ou placeholder não-bool | falha fechada antes da publicação | `tests/test_nextos_catalog.py` |
| `placeholder:false` | exige `--assets-root`; PNG/WebP/JPEG passam checks limitados de container/header/CRC/stream (não um decoder completo), SVG usa allowlist e conteúdo ativo/externo falha fechado; stdout registra path/SHA-256 pontual | `tests/test_nextos_catalog.py` |
| IP/path pessoal, markup ou origem pública proibida de APK | falha fechada antes da publicação | `tests/test_nextos_catalog.py` |
| JSON com chave duplicada, BOM ou NaN | falha fechada sem arquivo parcial | `tests/test_nextos_catalog.py` |
| fsync do diretório falha depois do rename | arquivo completo é removido pelo dirfd pinado e a falha não deixa output nem staging | `tests/test_nextos_catalog.py` |
| Geração normal sem chamar o exportador | catálogo continua separado; schemas nxproject e golden C3 permanecem estáveis, enquanto o golden C4 estático carrega o cabeçalho honesto da 0.3.12 | `tests/run-owner-catalog-final.sh` + `tests/test_c3_bytes_preserved.py` + `tests/test_c4_gptk_bytes.py` |

Limite desta owner branch: o JSON e seus links não foram consultados na rede,
o site não foi implantado e os hashes de assets são uma fotografia do checkout,
não campos do JSON editorial. `test_nxgenerator.py`,
`test_generation_runtime.py` e `test_package_payload.py` são preservados para a
bateria de integração com os siblings nxbootstrap 0.7.0/NXExtract 1.3.0; o gate
real HarbourMaster/seed da 0.3.2 não pode ser perdido nessa composição.

Acréscimos da integração 0.2.20 sobre a 0.2.19:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| Ciclo HarbourMaster REAL com seed | port schema 3 com `.nxb` passa autoinstall/uninstall/reinstall reais e o seed sai intacto; remover o seed do ZIP faz o ciclo real RECUSAR o port | `test_generation_runtime.py` |
| opt-ins V4 desligados por omissão | manifesto sem `display`/`egl_binding`/`sdl3_portmaster` normaliza para `game`/desligado; port aprovado regerado não muda de comportamento | `test_v4_declarative.py` |
| `display` finito | política desconhecida, variante de caixa, tamanho interno ausente/zero, limites em política errada, `adaptive` sem máximo, máximo abaixo do mínimo e par de limites pela metade falham fechado | `test_v4_declarative.py` |
| `egl_binding` exato | inventário vazio, sem `eglGetCurrentContext`, com símbolo não-EGL, fora de ordem, duplicado, ligado em port sem GL e desligado com inventário falham fechado | `test_v4_declarative.py` |
| `sdl3_portmaster` pinado | ligado sem SHA-256, dígito maiúsculo, truncado, e desligado com SHA falham fechado | `test_v4_declarative.py` |
| árvore corrente do framework | recibo declara nxgenerator 0.2.20 e nxbootstrap 0.6.37, com SHA-256 derivados do template e gerador reais | `test_generation_runtime.py` + `test_nxgenerator.py` |
| port já publicado com 0.2.19 | nenhum receipt, generation ID ou ZIP é reescrito; adoção continua opt-in | revisão de diff + ausência de alterações em `ports/**` |

Acréscimos preservados da ordenação canônica do inventário na 0.2.19:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `nxextract-version.txt` e `nxextract/run-extractor.sh` na mesma geração | `GENERATION.json.artifacts` ordena primeiro o arquivo com hífen, pela string POSIX estrita; renderer e validator consomem a mesma ordem | `test_generation_runtime.py` |
| paths com o mesmo prefixo seguidos por hífen, ponto, barra ou dígito, além de variação de caixa | ordem lexicográfica total pelo path lógico; modo e SHA-256 continuam associados ao membro correto | `test_generation_runtime.py` |

Acréscimos da composição autoral e tuning GPTK na 0.2.18:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| `package_payload` ordenado com docs autorais, notice 0644 e tool 0755 | mesmos bytes/modos no mesmo path abaixo do port; `kind` preservado no projeto e todo arquivo fixado no inventário final | `test_package_payload.py` |
| fonte muda depois da validação | materialização usa os bytes retidos já conferidos, sem reler a fonte nem trocar o SHA aprovado | `test_package_payload.py` |
| segunda geração com as mesmas fontes | árvores, modos, projeto e recibo byte-idênticos | `test_package_payload.py` |
| `documentation.status: authored` | payload inclui e substitui exatamente `README.md` + `INSTALLATION.md`, ambos `kind: payload`; ausência ou `license-notice` em qualquer um falha | `test_package_payload.py` |
| `documentation.status: scaffold` com README/INSTALLATION no payload | geração recusada; somente authored substitui documentação existente | `test_package_payload.py` |
| record com campo ausente/extra, kind/modo/hash inválido, array não-lista ou 129 arquivos | geração recusada antes da publicação | `test_package_payload.py` |
| path fora de ordem, duplicado por casefold, pai/filho, oculto, dot, traversal, barra invertida ou não-canônico | geração recusada antes de abrir/copiar payload | `test_package_payload.py` |
| fonte symlink, hardlink, modo divergente, >4 MiB ou conjunto >16 MiB | geração recusada; fonte precisa ser regular, link único e exatamente pinada | `test_package_payload.py` |
| ELF/`.so`, APK/APKM/APKS/XAPK ou archive por extensão/magic | geração recusada, inclusive archive/ELF renomeado | `test_package_payload.py` |
| path colide com launcher/executável/required/runtime, membro gerado ou namespace reservado | geração recusada; nenhum payload autoral sobrescreve runtime/estado | `test_package_payload.py` |
| `package_payload` contém `cover.png` | geração recusada; a imagem permanece de propriedade exclusiva do renderer como `portmaster-image` | `test_package_payload.py` |
| tool 0755 abaixo de `tools/`; 0755 fora desse namespace | primeiro materializa 0755; segundo falha fechado | `test_package_payload.py` |
| tuning completo de cursor/câmera nos limites | ordem fixa, decimal sem expoente e arquivo final aceito pelo parser C canônico do nxinput | `test_package_payload.py` |
| cursor vetorial `cursor.*` ligado em menu/gameplay, sem contexto cursor | bindings existentes preservados e `[cursor]` somente com tuning anexada | `test_package_payload.py` |
| contexto cursor com R3 sem prefixo `cursor.` | aceito quando RIGHT_STICK é vector, R3 é button e A/D-pad não são roubados | `test_package_payload.py` |
| cursor tuning sem ação vetorial `cursor.*` ligada; R3 não-button | geração recusada | `test_package_payload.py` |
| tuning desconhecido/vazio, bool-as-number, boolean/authority inválidos ou número fora do range | geração recusada | `test_package_payload.py` |
| schemas 1/2 ou schema 3 sem `package_payload`/`tuning` | caminhos legados e bytes GPTK anteriores preservados; campo novo em schema antigo falha | `test_package_payload.py` + `test_nxgenerator.py` |

Acréscimos da correção de composição real na 0.2.17:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| nxproject/nxport schema 3 + `generation_runtime` + NXExtract ativo | pasta e cinco membros NXExtract já materializados pelo nxbootstrap são reutilizados com bytes/modos idênticos; geração v2 é publicada | `test_generation_runtime.py` |
| membro NXExtract pinado e internamente consistente, porém diferente do canônico | geração recusada antes da publicação; nxgenerator não sobrescreve o membro para mascarar a divergência | `test_generation_runtime.py` |

Acréscimos da integração generation-runtime na 0.2.16:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| nxproject v3 + nxport v3 com `generation_runtime` + `runtime_root` válido | nxbootstrap 0.6.37 recebe a raiz; executável, biblioteca, hook e NXSplash aparecem com bytes/modos exatos na raiz ativa e na geração v2 imutável | `test_generation_runtime.py` |
| `runtime_root` ausente ou diretório inexistente | geração recusada antes de publicar qualquer árvore | `test_generation_runtime.py` |
| membro de runtime com SHA-256 divergente | geração recusada pelo contrato canônico do nxbootstrap | `test_generation_runtime.py` |
| `runtime_root` em projeto/nxport legado | geração recusada; a entrada nunca é silenciosamente ignorada | `test_generation_runtime.py` |
| nxproject/nxport schema 2 sem `runtime_root` | saída continua schema 2, generation-runtime ausente e `.nxruntime` permanece no formato v1 | `test_generation_runtime.py` + `test_nxgenerator.py` |
| closure v2 gerada | `manifest.json`, `components.v2`, `identity.json`, `identity-runtime.v2`, `format` e `commit` concordam com o mesmo generation ID | `test_generation_runtime.py` + validação interna do nxgenerator |
| qualquer nxproject válido | `gameinfo.xml` 0644 contém exatamente um path/nome derivados de launcher/title, com escape XML, e entra no inventário da geração | `test_generation_runtime.py` + `test_nxgenerator.py` |
| renderer de `gameinfo.xml` adulterado | geração recusada antes da publicação | `test_generation_runtime.py` |

Acréscimos V3 preservados da 0.2.15:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| Projeto com NXExtract | `gamedata/README.txt` real, regular, 0644, bilíngue, sem origem/filename; `INSTALLATION.md` documenta `ports/<id>/gamedata/` | `test_nxgenerator.py` |
| Projeto sem NXExtract | nenhuma promessa automática de `gamedata/` | `test_nxgenerator.py` |
| `owner_data` v3 | bloco único deriva pasta/formatos/versão do marcador; diretório não-canônico e v2 com owner_data falham | `test_nxgenerator.py` |
| Receita com `search_dirs` divergente | `gamedata` fora da primeira posição falha fechado antes de gerar | `test_nxgenerator.py` |
| Port novo | `defaults/NEXTOSCONTROLLERS.gptk` NEXTOS_CONTROLLERS/1 imutável presente | `test_nxgenerator.py` |
| Port V3 com controles próprios | GPTK renderizado só das ações declaradas; cada ação possui tipo e sink; mapping SDL completo é autoridade | `test_nxgenerator.py` |
| Contrato de controles inválido | ausência do bloco, ação/sink desconhecido ou duplicado, tipo incompatível e cursor que rouba A/D-pad falham fechado | `test_nxgenerator.py` |
| Recibo | `GENERATION.json.generation_id` é SHA-256 determinístico completo de 64 hex | `test_nxgenerator.py` |
| Container por identidade | sha256/crc32 em QUALQUER quantidade e size exato recusados (apkcompat 1.0.0) | fixtures compartilhadas + `test_nxgenerator.py` |
| Projeto sem `promotion` | adapter continua `unimplemented_nonrelease`; claims de release e suporte físico continuam falsos | `test_nxgenerator.py` |
| `promotion` v3 coerente | contrato real `implemented_release` é materializado; claims explícitos são reproduzíveis em duas gerações | `test_nxgenerator.py` |
| `promotion` em schema antigo, path inseguro, claim contraditório ou contrato divergente | geração recusada antes de publicar a árvore | `test_nxgenerator.py` |
| Schema v3 | propriedade `promotion` fechada, sem campos extras e com tipos/constantes dos três claims | `test_nxgenerator.py` + `nxproject-v3.schema.json` |

Herdada da 0.2.11:

| Entrada | Resultado obrigatório | Prova |
|---|---|---|
| NXExtract ativo | engine 1.2.21 completo e UI gráfica 1.2.16 imutável, 0755 e pinada separadamente | `test_nxgenerator.py` |
| UI por papel | legado seleciona pela ABI do jogo; mixed seleciona NXExtract UI pela ABI de extractor, nunca bytes cruzados | `test_nxgenerator.py` + manifesto NXExtract |
| Splash por papel | legado seleciona pela ABI do jogo; mixed seleciona NXSplash pela ABI de splash | `test_nxgenerator.py` + manifesto NXSplash |
| Receipt mixed ABI | papéis completos e pins independentes de extractor/splash registrados; projeto v1 com roles recusado | `test_nxgenerator.py` |
| Release UI adulterado | versão, path/bytes cruzados, SHA, source, toolchain ou GLIBC divergente falham fechado | `test_nxgenerator.py` |
| Ambiente sem `stat` | launcher e helpers públicos gerados não chamam o comando externo | gate local + suíte `nxbootstrap 0.6.37` |
| Port standalone | raiz explícita real gera sem depender do monorepo e sem vazar path host; symlink falha | `test_nxgenerator.py` |
| NXExtract desativado | nenhum arquivo NXExtract/UI | `test_nxgenerator.py` |
| APK-container com um SHA/tamanho | geração recusada | `test_nxgenerator.py` |
| APK-container por conteúdo | package + SHA interno forte ou árvore estrutural forte aceitos; bounds/magic externos opcionais | `test_nxgenerator.py` + fixtures Angry/Retro/Scourge |
| Scaffold repetido | árvores byte-idênticas | `test_nxgenerator.py` |
| Metadata PortMaster v2 | `runtime` é lista explícita e o recibo fixa schema/contrato/parser | `test_nxgenerator.py` + `test_portmaster_contract.py` |
| Runtime ausente/nulo/escalar/duplicado | geração recusada antes da publicação | `test_nxgenerator.py` |
| JSON com BOM/duplicata/NaN/vírgula final/truncamento | geração recusada sem árvore parcial | `test_nxgenerator.py` |
| Manifesto legado v1 | entrada histórica continua aceita e emite `runtime: []`; mixed ABI exige v2 | `test_nxgenerator.py` |
| ZIP sintético gerado | parser upstream instala, descobre, remove e reinstala sem rede nem guest | `test_nxgenerator.py` + `harbourmaster-cycle.py` |
| Instalação pública | `INSTALLATION.md` bilíngue sempre existe e nasce fail-closed como scaffold | `test_nxgenerator.py` |
| Ordem de abertura | NXExtract/UI → payload → logo → adapter → jogo | deployment tamper gate |

A matriz não declara suporte físico. Cada port ainda precisa optar pela versão,
gerar novo ZIP e provar os mesmos bytes no hardware autorizado.

O recibo isolado 1.2.9 no NextOS Mali-450 inclui captura gráfica válida do
renderer SDL `mali`. O alvo ArkOS-class/KMSDRM comprovou apenas `visible=sdl`,
log do renderer e recuperação EGL/GLES; `/dev/fb0` continha quadro antigo e não
é prova visual. Nenhum desses recibos afirma instalação ou execução de jogo.

## V4-GRAPHICS-04 (0.3.1)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| evidence_boundary ausente | O campo nunca é inventado; `adapter-contract.json` e todos os bytes gerados ficam idênticos aos da 0.3.0 | `tests/test_nxgenerator.py` |
| evidence_boundary declarado | `post-first-present` round-trips literal no bloco `graphics` do `adapter-contract.json` | `tests/test_nxgenerator.py` |
| Negativos | Valor desconhecido e declaração em port `uses_gl:false` falham fechados | `tests/test_nxgenerator.py` |

## V4-CONTROLLERS-03/C3 (0.3.2)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| controller_profiles round-trip | Pin literal em `input_controller_profiles`; sem declaração o campo NÃO existe | `tests/test_nxgenerator.py` |
| Bytes preservados sem opt-in | Contrato regenerado byte a byte igual ao golden real do nxgenerator 0.3.1 (`1c83799`); o opt-in não altera mais nenhum byte | `tests/test_c3_bytes_preserved.py` + `tests/fixtures/c3-baseline/` |
| Negativos | enabled sem bundle/pin, path separator, disabled com pin, digest ausente falham fechados | `tests/test_nxgenerator.py` |

## V4-CONTROLLERS-03 / C4 (0.3.2) — NEXTOSCONTROLLERS v2

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Formato sem opt-in | magic e corpo de bindings permanecem em v1; desde 0.3.12 o golden usa o cabeçalho estático verdadeiro em vez da promessa editável | `tests/test_c4_gptk_bytes.py` + `tests/fixtures/c4-baseline/` |
| 18 campos em schema 2 | toda seção lista os 18 controles, uma vez, na ordem estável | `tests/test_c4_gptk_bytes.py` |
| `null` / `native` | não usado vira `null`; passthrough declarado vira `native` | `tests/test_c4_gptk_bytes.py` |
| Negativos | schema desconhecido, `native` sem opt-in, `null` escrito no manifesto falham fechados | `tests/test_c4_gptk_bytes.py` |
## GPTK vivo fail-safe (0.3.9)

| Caso | Classe | Resultado obrigatório |
|---|---|---|
| opt-in `nxinput-gptk` | host/puro | gera `nxinput-gptk-live/1` completo |
| sem opt-in | host/puro | não serializa `runtime_contract` |
| adapter promovido omite/relaxa política | host/puro | falha fechado |
| contexto desconhecido | runtime/contrato | passthrough nativo, zero supressão |
| sink sem ACK | runtime/contrato | candidato fatal, nunca verde |

## GPTK V3 / FACE_LAYOUT (0.3.15)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| `tests/test_c4_gptk_bytes.py` | goldens V1/V2 byte-idênticos; V3 rende `FACE_LAYOUT` exatamente uma vez no preâmbulo com completude de 18 controles; negativos p/ schema desconhecido, case errado e face_layout sem schema 3 | pure |
| `tests/test_face_layout_variants.py` | par modern/retro completo obrigatório sob schema 3 + profiles habilitado; nomes fixos; pins distintos; nada V3 em schema 1/2; bloco desabilitado não carrega variantes | pure |
