# 0.4.5 (2026-09-04, V5: algoritmo declarativo `auto-stretch`)

- O enum fechado `video.auto_algorithm` ganha `stretch`, correspondente ao
  algoritmo total `nxcompat_video_auto_stretch()` 0.5.3. Ele mantém o default
  aprovado de tela cheia em qualquer drawable; `video.aspect=preserve`
  continua sendo a escolha explícita do dono para letterbox.
- Não existe selector por modelo, CFW ou resolução nominal. Os valores antigos
  `ratio-threshold`, `epsilon` e `none` permanecem literais e inalterados.
- `tests/test_v5_owner_video.py` cobre o positivo e recusa um falso algoritmo
  por aparelho.

# 0.4.4 (2026-09-04, V5: canal do gatilho decidido por TODOS os contextos)

- Campo Nameless Cat 1.2.8 (04/09): o dono pediu o clique do cursor também no R2, só no menu
  (em gameplay o cursor não existe). Com R2 = null na base (gameplay) e `cursor.click` no
  override do menu, o 0.4.3 escolhia o canal pelo binding da BASE e escrevia
  `trigger.right.analog = action:cursor.click` — kind mismatch que o parser do schema 4 recusa
  (`NXINPUT_GPTK4_ERR_KIND`); o default nem carregava. O canal (digital/analógico) passa a ser
  decidido pelo conjunto dos contextos: um botão em qualquer contexto = `mode = digital` na
  seção `[trigger.*]` e override no canal digital; botão E analógico no mesmo gatilho em
  contextos diferentes = `ProjectError`, nunca silêncio. Teste:
  `tests/test_v5_gptk4_trigger_override.py` (mutante: canal só pela base).

# 0.4.3 (2026-09-04, V5: START isolado ANTES dos sticks)

- Campo FP2 (03/09, noite): mover o jogador dispara diálogo/cutscene roteirizado (tutorial) e o
  "skip" consome o toque de pausa seguinte — o START isolado, que vinha DEPOIS das gestos de stick,
  nunca via seu efeito (`context_change`) na janela. O bloco de START (delivery, press/release
  uma vez, efeito declarado, segundo toque para desfazer) passa a vir logo depois dos botões
  discretos e antes dos sticks; continua antes do hotplug e dos negativos. Teste:
  `tests/test_input_proof_roteiro.py` (mutante: START depois do stick).

# 0.4.2 (2026-09-03, V5: a prova exige o EFEITO da engine, não só o delivery do adaptador)

- **Defeito que motivou (FP2 1.1.4, 03/09 noite).** Um hook IL2CPP na função de POLLING do jogo
  (`JoystickInput.getInputName` → `Input.GetButton`) matou todos os botões; o receipt do adaptador
  continuou registrando `delivery` normal e a sessão "menu" deu ALL PASS. O oráculo contava só a
  palavra do adaptador.
- `controls.proof.effects` (opcional): `{ "<action>": { "context": "<contexto>", "source_regex": "<regex>" } }`
  declara o contexto que a ENGINE publica depois da ação (o receipt `kind=context` do adaptador). O
  gerador emite `expect: context_change` logo após cada `delivery` daquela ação (cobertura por
  contexto, START isolado e owner-remap). Declarar só ações cujo efeito deixa a sessão onde a
  cobertura gerada espera (START que pausa é pressionado de novo pelo roteiro; um confirm que troca
  de tela NÃO é auto-reversível e fica sem efeito declarado). Validação fail-closed: ação e contexto
  declarados, regex válida, sem membros desconhecidos. Requer nxinput 0.11.6 (`nx-device-input-proof.py`
  com `context_change`).
- Teste: `tests/test_input_proof_roteiro.py` (mutante: delivery de START aceito sem a engine provar a pausa).

# 0.4.1 (2026-09-03, V5: a closure esperada nasce do PROJETO)

- **Defeito que motivou (M1c, item 1).** O harness de host do Tearscape
  carregava a própria expectativa dentro do port. Quando o owner virou schema
  4, o harness ficou no loader V1–V3 e a prova inteira foi a vermelho já em
  `generated default map loads` (`NXI1006`), aparecendo três cascatas depois
  como "generated vector reaches the declared action sink" — a linha que matou
  o laço da rodada 3 sem deixar contexto (E11 da auditoria). O template e o
  bridge estavam certos (15/15 na sonda); o gate de prova é que ficou no V3.
- Regra 8.1: o esperado nunca vem do mapa sob teste. O gerador, que já é dono
  de `controls.contexts` e `controls.actions`, passa a escrever
  `CONTROLS-CLOSURE.json` (`nx-controls-closure/1`) ao lado do owner gerado,
  no mesmo formato que o `make_input_proof.py` calcula: `context`, `control`,
  `event` (press/axis/motion), `decision=ACTION`, `action`, `sink`,
  `delivery_count`. Um harness de host passa a LER a closure em vez de a
  recalcular.
- `declared_contexts` sai junto de propósito: o `[base]` do schema 4 é
  unificado por contrato e alcança contextos que o port nunca declarou — uma
  binding que chega lá NÃO é binding extra. Foi essa confusão que fez a
  closure do harness do Tearscape exigir `CURSOR`, contexto inalcançável
  naquele port.
- `native` e `null` não geram caso: são a ausência de ação, provada pelo gate
  de host como NONE/SUPPRESS. Binding para ação não declarada é recusada e
  nomeada.
- Gate novo `tests/test_controls_closure.py` (12 checks, 2 mutantes).

# 0.4.0 (2026-09-03, V5 7A: bloco `video` (NEXTOS_SETTINGS/2), `owner_runtime`, manifesto de ownership)
- `controls.schema: 4` (NEXTOS_CONTROLLERS/4, nxinput 0.11.0): `[base]` unificado a
  partir do contexto `gameplay`, `[override.<ctx>]` só com os controles que
  divergem, `[stick.*]`/`[trigger.*]` normativos, `GUIDE` explícito, bindings
  tipados `action:<id>|native|null`; exige `runtime_mapping: nxinput-gptk` e
  recusa `face_layout` (a superfície é posicional Xbox). O default gerado para
  o FP2 está pinado byte a byte no corpus do nxinput
  (`tests/v5/corpus/fp2-generated-v4.gptk`, gate `nxgenerator-v5-owner-video`),
  e o parser C real o aceita (gate `nxinput-v5-host`, `nx-gptk4-check`).

- `video` (nxproject schema 3, opcional; AUSÊNCIA = no-op byte-idêntico): o port
  declara `authority` (nextos|engine|synchronized), as `aspect_policies` que
  REALMENTE implementa (auto|engine|preserve|stretch|crop|integer), o
  `auto_algorithm` TOTAL quando `auto` está na lista (ratio-threshold|epsilon;
  `auto` sem algoritmo é recusado, algoritmo sem `auto` também), o `aspect`
  default (obrigatoriamente entre as policies declaradas — nunca fallback
  fingido), `output_size` (auto|display|presets|WxH ≤ 8192), `filter`,
  `invalid_policy` (fail_closed|last_known_good|package_default) e
  `native_config` (obrigatório em authority engine/synchronized: o arquivo
  owner-native da engine, ex. `game/override.cfg`). Nenhum campo de device/CFW.
- Com `video`, `defaults/NEXTOSSETTINGS.txt` nasce em `# NEXTOS_SETTINGS/2`
  (template `NEXTOSSETTINGS-2.txt.in`) com TODAS as chaves `video.*` explícitas
  e comentadas bilíngue; sem `video`, o seed `/1` continua idêntico.
- `owner_runtime` (projeto): `{"hook_seed": <arquivo regular no source_root>}`
  — o seed do hook vivo, copiado para `defaults/port-env.sh` (nunca para o
  path vivo). Exige `nxport.owner_runtime: "1"` (nxbootstrap 0.8.0) e
  vice-versa; symlink recusado.
- `OWNERSHIP.json` (`nx-ownership/1`) no pacote: classifica cada path como
  `owner-seeded` (defaults→live, nunca curado), `sealed-runtime`
  (`adapter-env.sh`) ou `owner-native` (native config declarado, com a
  authority), e registra a ordem observável
  heal-sealed→extract→splash→seed-owner→sealed-env→owner-env→launch.
- Gate novo `nxgenerator-v5-owner-video` (positivos, mutantes: auto sem
  algoritmo, default fora das policies, authority engine sem native_config,
  campo de CFW, seeds simbólicos/desalinhados). Pins de versão dos testes
  sobem para 0.4.0. A composição V5 repina o template do nxbootstrap 0.8.0.

# 0.3.17 (2026-09-02, roteiro: START isolado antes dos negativos)

- `render_input_proof_roteiros`: a verificação do START (entrega da ação
  ligada, press/release uma vez) passa a vir ANTES do hotplug e dos negativos,
  e um START que chegou à engine é pressionado de novo para desfazer um pause
  antes de os negativos rodarem. Na 0.3.16 o negativo `chord_cross`
  SELECT+START entregava START puro ao jogo (a união preserva os botões; só o
  chord é negado): um START ligado a pause já tinha trocado o contexto quando o
  roteiro media a entrega "no contexto coberto" — falso FAIL medido no Nameless
  Cat 1.2.7 (entrega em `menu source=ui:modal` em vez de `gameplay`).
- Sem mudança de schema; `generated_by` passa a `nxgenerator 0.3.17`.

# 0.3.16 (2026-09-02, roteiro automático da prova de controles no aparelho)

- `controls.proof` (opcional, schema 3): o port fornece SÓ o que o framework
  não pode saber — `navigation` (passos até cada contexto declarado),
  `quit_guard` (controle do menu que nunca pode selecionar QUIT) e
  `owner_remap` (controle que vira `null` e para onde a ação vai). Validado,
  mas fora do adapter-contract (não muda a promoção).
- Para um port com `controls.proof`, o gerador escreve ao lado do pacote
  (`<output>-proof/`, nunca dentro do ZIP) os roteiros
  `nx-device-input-proof-roteiro/1` do nxinput 0.10.2: `input-proof-default.json`
  (todo controle ligado → entrega + press/release uma vez; `null` → supressão;
  `native` → passthrough; sticks → vetor e volta ao neutro; neutralidade;
  L1+R1, L2+R2, SELECT sozinho, START e SELECT+START cross-pad não encerram;
  SELECT+START no mesmo instance encerra limpo), `input-proof-owner-remap.json`
  e `owner-remap-NEXTOSCONTROLLERS.gptk`. Próximos ports V4 herdam a prova
  declarando o membro; ports antigos aprovados não migram.
- Gate novo `tests/test_input_proof_roteiro.py` (`nxgenerator-input-proof-roteiro`).

# 0.3.15 (2026-09-01, GPTK V3 / FACE_LAYOUT e o par de variantes de bundle)

- `controls.schema` aceita 3 (NEXTOS_CONTROLLERS/3, nxinput 0.10.0): herda a
  completude/tri-state do V2 e renderiza exatamente uma linha
  `FACE_LAYOUT = auto|modern|retro` no preambulo, deterministica. O default e
  `auto`; `controls.face_layout` (somente schema 3) escolhe outro valor.
- `controls.controller_profiles` ganha o bloco opt-in `face_layout_variants`
  (somente schema 3): o par COMPLETO modern/retro, nomes fixos
  `controllers-modern.nxb`/`controllers-retro.nxb`, um SHA-256 proprio por
  variante, pins distintos. Schema 3 com profiles habilitado EXIGE o par;
  schema 1/2 nao aceita nenhum campo V3 (oraculo selado N10-N12).
- Goldens V1/V2 permanecem byte-identicos (gate C4 reafirmado no mesmo
  commit); nenhum port antigo e regenerado ou migrado por conta propria.
- Schema JSON nxproject-v3 atualizado de forma aditiva; gate novo
  `tests/test_face_layout_variants.py` registrado na matriz oficial e no
  run-safe-gates.

# 0.3.14 (2026-08-31, pré-verificação da autoridade NXExtract — V4-04C)

- **Nenhum byte do engine executa antes da prova de identidade.** O
  `exec_module()` do 0.3.13 executava `nxextract.py` antes de validar hash e
  versão; uma árvore adulterada podia rodar código antes da recusa. Agora o
  caminho canônico é derivado só dos componentes fixos do repositório
  (candidato/source root/env jamais escolhem módulo), cada componente é
  validado por `lstat` (diretório real, nunca symlink), o arquivo é aberto
  com `O_NOFOLLOW`, validado por `fstat` (regular, teto de 4 MiB, mesmo
  inode/device do lstat), lido UMA vez, e o SHA-256 dos bytes apresentados
  precisa igualar a identidade pinada independente
  (`NXEXTRACT_ENGINE_SHA256`); "hash calculado agora" nunca é autoridade de
  si mesmo.
- A versão é extraída por AST somente dos bytes verificados: exatamente uma
  atribuição top-level literal `NXEXTRACT_VERSION`; expressão, duplicata,
  ausência ou divergência (também contra o `VERSION` canônico, lido com a
  mesma segurança) falham ANTES de executar.
- Só depois dessas provas os bytes já verificados são compilados e
  executados sob o nome lógico estável `<nxextract-recipe-authority>`; o
  pathname nunca é reaberto para execução, então troca entre verificação e
  uso não executa bytes novos.
- A autoridade só entra no cache após a interface `Recipe`/`NXError`
  aprovada; qualquer falha deixa `_NXEXTRACT_AUTHORITY` vazio.
- Mensagens externas determinísticas e sanitizadas: sem pathname pessoal,
  sem traceback, sem conteúdo hostil.
- Paridade de receita do 0.3.13 preservada (mesmo corpus, mesmas decisões);
  NXExtract permanece 1.3.0 byte-idêntico
  (`e59a1e525ea475635c2e8f51bff3c6fff8cdaad406c158e3dad011cdd96876a7`).

# 0.3.13 (2026-08-31, receita delegada à autoridade NXExtract — V4-04B)

- O validador isolado de receita do generator (parse JSON + root object) é
  substituído por delegação integral à autoridade canônica: a classe
  `Recipe` do NXExtract **1.3.0**, carregada SEMPRE do engine do framework
  (`suportando_outros_devices/extrator-universal/nxextract.py`), jamais de
  código vindo do candidato/source_root. Antes desta versão o generator
  aceitava receita estruturalmente inválida (sem `commit`, sem `extract`,
  `validate` com tipo errado, chave duplicada, BOM…) que o NXExtract recusa
  no device — a divergência foi provada por controle negativo end-to-end
  contra o 0.3.12.
- O gate de versão é duplo e fail-closed: `NXEXTRACT_VERSION` do engine e o
  arquivo `VERSION` do NXExtract precisam ambos ser exatamente 1.3.0
  (`NXEXTRACT_RECIPE_AUTHORITY_VERSION`); drift reprova antes de ler receita.
  `RecursionError` (nesting abusivo) vira `ProjectError`, nunca traceback, e
  as mensagens trocam o caminho do host pelo valor lógico do manifesto.
- Políticas próprias do generator permanecem ADITIVAS e provadas
  não-contraditórias pela matriz: flexibilidade APK-variant (contrato
  apkcompat 1.1.0, preservado) e `input.search_dirs` com `gamedata` primeiro.
  O NXExtract 1.3.0 não muda em nada (bytes, comportamento, UI).
- Gate novo `tests/test_recipe_authority.py` + corpus versionado
  `tests/fixtures/recipe-authority-corpus/` (21 casos: validate
  ausente/vazio/cada tipo inválido, extract/commit ausentes/errados, receita
  mínima e realista, unknown fields tolerados, duplicata, BOM, UTF-8
  inválido, NaN em campo tipado vs. campo desconhecido, root não-objeto,
  schema errado; oversize e nesting profundo gerados deterministicamente):
  paridade generator×autoridade por caso, identidade de bytes do engine por
  SHA-256, gate de versão e sanitização de caminho.

# 0.3.12 (2026-08-31, mapa nativo estático sem promessa editável)

- Corrige a contradição entre gerador e release para ports com controles
  nativos: sem `controls.runtime_mapping`, `NEXTOSCONTROLLERS.gptk` agora diz
  explicitamente que é um mapa estático e que editar a cópia não altera a
  engine. O nxrelease já recusava corretamente a promessa editável sem runtime.
- O opt-in `controls.runtime_mapping = "nxinput-gptk"` preserva byte a byte o
  cabeçalho editável anterior e todas as provas de loader/dispatcher, sinks,
  ACK e autoridade 3. Schema 1/2, ações, contextos e corpo dos bindings não
  mudam.
- A adoção é somente pela versão 0.3.12 em candidato novo; nenhum ZIP aprovado
  é regenerado ou migrado automaticamente.

# 0.3.11 (2026-08-31, composição corrente nxbootstrap 0.7.8)

- O nxgenerator 0.3.11 já deriva versão e hashes do nxbootstrap diretamente
  de `VERSION` e dos sources; por isso a composição passa a 0.7.8 sem alterar
  lógica, schema ou identidade do gerador.
- A nova closure permite `private-library` 0644 sob NXExtract ativo; validação,
  store e launcher pertencem ao nxbootstrap 0.7.8. Receipts passam a fixar os
  novos bytes exatos de `generate-port.py` e `launcher.sh.in`.

- Registro inicial: a mesma lógica foi integrada primeiro sobre nxbootstrap
  0.7.7, cuja promoção de
  geração exige status 0 do filho além de health e vídeo exatos.
- Atualiza versão, receipts e pin byte-exato do template. Schemas declarativos,
  autoridade 3 `controllers.nxb` e comportamento sem opt-in não mudam.
- Fecha a substituição por `promotion`: o adapter owner precisa conter
  `input_controller_profiles` exatamente igual ao pin normalizado de
  `controls.controller_profiles`; campo ausente, stale ou inventado falha
  antes da publicação. O contrato promovido continua copiado verbatim.

# 0.3.10 (2026-08-31, autoridade 3 obrigatória no runtime nxinput)

- **`controls.runtime_mapping = "nxinput-gptk"` agora exige
  `controls.controller_profiles` habilitado** (bundle NXCONTROLLER_PROFILES/1
  pinado dentro do ZIP). Uma falha de campo publicou um port sem bundle e a
  ordem de autoridades perdeu o próprio degrau de resgate quando as fontes do
  CFW eram insuficientes. Port sem runtime nxinput não recebe campo nem
  comportamento de controle novo; a identidade da geração registra 0.3.10.
- Gate dirigido cobre ausência, bundle desabilitado e bundle válido antes de
  qualquer geração; sem `runtime_mapping`, payload e política de controles
  permanecem inalterados além do pin/receipt da nova versão.
- O bundle habilitado usa obrigatoriamente o nome canônico `controllers.nxb`,
  que é o caminho declarado pelo runtime NXC6. Renomeá-lo não pode mais passar
  geração e perder silenciosamente a autoridade 3 durante o boot.

# 0.3.9 (2026-08-31, GPTK vivo fail-safe)

- O opt-in `controls.runtime_mapping = "nxinput-gptk"` passa a gerar também
  `input.runtime_contract` no adapter: contexto inicial `unproven`, política
  `native-passthrough`, cobertura de todos os ACTIONs antes da ativação e ACK
  de entrega obrigatório. O port não pode selecionar gameplay por omissão nem
  autorizar supressão antes dessas provas.
- Um adapter promovido precisa copiar esse contrato exatamente; omissão ou
  relaxamento falham na geração. Ports sem o opt-in não ganham o campo e
  preservam os bytes anteriores.
- Gate dirigido `test_gptk_live_contract.py` fixa a forma canônica e o caminho
  sem opt-in. O nxrelease 0.3.12 fecha a declaração contra o ELF e a evidência
  externa evento → decisão → sink → ACK.

# 0.3.7 (2026-08-30, provider SDL do sistema e prova de video obrigatoria)

- Adota o `nxbootstrap 0.7.4` e transporta sem reinterpretação os opt-ins
  fechados de `nxport`: `sdl_provider: "system"` e
  `video_proof: "required"`.
- O primeiro fixa o provider SDL na integração do firmware/PortMaster, sem
  selecionar backend por nome de device; o segundo exige receipt run-bound de
  pixels não pretos antes de a geração poder receber health.
- Ausência continua sendo ausência: nenhum dos campos é inventado ou
  serializado como `null`. Comparando duas gerações na mesma composição 0.3.7,
  o contrato do adapter, controles, documentação e demais payloads alheios aos
  dois opt-ins preservam bytes.
- Novo gate dirigido cobre round-trip no `nxproject` e no `nxport` gerado,
  ativação no launcher, pin exato do bootstrap, identidade distinta, valores
  inválidos e preservação literal sem declaração. Ele não usa rede, device,
  screenshot DRM/GBM nem áudio como substituto de imagem.
- O sucessor final fixa os sources auditados do `nxbootstrap 0.7.4`
  (`generate-port.py` SHA-256 `7e559ba68ff4ac75d2e475642b6aab942e2f1991623e7aa08c1307262d807483`;
  template SHA-256 `ea5d3253de923a47b28826792ce88403e39c00902e658ff32682aae9ed83be6a`)
  e torna o gate provider/vídeo requisito automático do gate existente de
  generation-runtime, com stdout capturado e diretórios temporários isolados.

# 0.3.6 (2026-08-30, integração V4 aberta: runtime gerenciado + catálogo)

- Composição das duas linhas paralelas da 0.3.2: o exportador opt-in de
  catálogo do site (0.3.3) e o runtime gerenciado/PortMaster v3 (0.3.4/0.3.5).
  Nenhum comportamento muda; o receipt da geração passa a identificar 0.3.6.
- Adota o nxbootstrap 0.7.3 (composição runtime-data + rollback real-crash).

# 0.3.5 (2026-08-30, runtime gerenciado na geração V2)

- Integra o contrato aditivo `runtime-data` do nxbootstrap 0.7.2.
- Prova a materialização byte a byte de assembly gerenciado na raiz ativa e
  no store imutável, com receipt do nxgenerator 0.3.5.
- Preserva a omissão compatível de `portmaster.runtime: []` introduzida na
  0.3.4 e não regenera nenhum port sem opt-in.

# 0.3.4 (2026-08-30, compatibilidade PortMaster de runtime vazio)

- Adota `framework/portmaster` 2.1.0, contrato v3 e metadata schema v2, com
  proveniência do HarbourMaster atual e da tag oficial legada
  `2024.03.10-0841`.
- Mantém `portmaster.runtime` obrigatório e estritamente validado no projeto,
  mas omite `attr.runtime` do `port.json` v4 quando a lista é vazia. Isso evita
  o crash do instalador legado e continua normalizando para `[]` no parser atual.
- Preserva literalmente listas não vazias e acrescenta regressões para os dois
  caminhos. Nenhum port já aprovado é regenerado automaticamente.
- O número 0.3.3 já identifica o owner de catálogo V4; esta correção independente
  parte da composição integrada 0.3.2 e usa o sucessor 0.3.4 sem incorporar o
  catálogo ainda não integrado.

# 0.3.3 (2026-08-30, V4-NXGENERATOR-NEXTOS-CATALOG-01)

- Novo `catalog_export.py`, opt-in e separado da geração runtime, que recebe a
  ficha editorial no formato exato fornecido por Ronax e publica JSON UTF-8
  determinístico sem sobrescrever destino existente.
- Novo contrato fechado
  `schema/nextos-port-catalog-v1.schema.json`, sem injetar `schema`, bindings
  ou extensões na saída que o consumidor atual não pediu.
- Binding cruzado fail-closed: `name` acompanha `nxport.title` (diferença de
  caixa permitida), o slug do repositório acompanha `id`, a release pertence ao
  mesmo repositório e baixa exatamente `<nxport.id>.zip`; ports com NXExtract
  citam o caminho canônico `<nxport.id>/gamedata/` no resumo.
- URLs são exclusivamente HTTPS sem credenciais/porta/query/fragmento/IP/local,
  imagens ficam sob `port-json/<id>/`, a build usa SemVer e tipo finito
  (`alpha`/`beta`/`rc`/`stable`), e textos públicos passam pelos gates de
  privacidade/origens proibidas.
- Leitura JSON estrita e retida por descritor rejeita symlink, hardlink, BOM,
  duplicatas, NaN e mudança durante leitura; arquivos acima de 64 KiB falham.
- Novo gate `tests/test_nextos_catalog.py` com o exemplo literal de Ronax pelo
  CLI completo, determinismo, modo `0644`, quatro fixtures válidas passando
  checks limitados de formato, seis SVGs ativos recusados, rollback pós-rename
  e 46 negativos. Para assets
  não-placeholder, o CLI emite receipt pontual path/SHA-256 sem alterar o shape
  editorial. A geração normal, os schemas nxproject e os goldens C3/C4
  permanecem byte-idênticos à 0.3.2.
- Escopo honesto: fixture Ronax é somente prova de formato; zero rede, site,
  release, device ou existência remota verificada. Os gates de composição que
  exigem siblings V4 permanecem pendentes para a integração, onde o gate
  HarbourMaster/seed herdado da 0.3.2 precisa ser preservado.

# 0.3.2-C4 (2026-08-29, V4-CONTROLLERS-03/C4: NEXTOSCONTROLLERS v2 opt-in)

> Mesma versão 0.3.2 (linha ainda não lançada): a missão 115 exige manter a
> versão proprietária e não criar sucessor só para esta categoria.

- Novo opt-in `controls.schema` (1 ou 2; ausência = 1). Em **schema 2** o
  `defaults/NEXTOSCONTROLLERS.gptk` passa a listar **os 18 controles** em cada
  seção declarada, na ordem estável: quem o port usa recebe a ação (ou o
  `native` explícito) e quem ele não usa recebe `null`. O dono enxerga o pad
  inteiro.
- `native` é aceito como valor de binding **somente** em schema 2; ele não tem
  sink por construção (o adapter lê o controle) e por isso não é conferido
  contra a allowlist de ações nem contra o `kind`.
- O manifesto **não** escreve `null` à mão: um controle que o port não usa é
  simplesmente omitido e o gerador emite `null` por ele. Escrever `null` no
  manifesto falha fechado.
- **Sem o opt-in nada muda**: a magia renderizada continua
  `NEXTOS_CONTROLLERS/1` e a saída é byte a byte igual ao baseline anterior à
  C4 — igualdade literal provada por `tests/test_c4_gptk_bytes.py` contra um
  golden real em `tests/fixtures/c4-baseline/`.
- Negativos fail-closed: `controls.schema` desconhecido, `native` sem o
  opt-in, `null` escrito no manifesto.

# 0.3.2 (2026-08-29, V4-CONTROLLERS-03/C3: controls.controller_profiles)

- Schema 3 ganha o opt-in `controls.controller_profiles` — o port pode fixar
  o SHA-256 do bundle `NXCONTROLLER_PROFILES/1` (`controllers.nxb`) que
  embarca no ZIP, autoridade 3 da ordem soberana de mapping do nxinput.
  `bundle` é nome de arquivo simples dentro do port (sem separador de path);
  nunca `latest`, nunca download em runtime.
- O `adapter-contract.json` carrega `input_controller_profiles` **apenas
  quando o port declara** o opt-in (missão 114A). Sem declaração a saída fica
  **byte a byte igual ao baseline anterior à C3** — provado literalmente
  contra um golden real gerado pelo nxgenerator 0.3.1 (`1c83799`) em
  `tests/fixtures/c3-baseline/`, por `tests/test_c3_bytes_preserved.py`.
  Acrescentar um bloco desabilitado a todo contrato regenerado, como fazia a
  primeira versão da C3, contradizia a preservação de bytes prometida aos
  ports já publicados.
- Negativos fail-closed: enabled sem bundle/pin, path traversal, disabled com
  pin, campo ausente.

# 0.3.1 (2026-08-29, V4-GRAPHICS-04: graphics.evidence_boundary)

- Schema 3 ganha o campo opcional `graphics.evidence_boundary`, com um único
  valor inicial: `post-first-present`. **Ausência preserva a fronteira
  anterior e os bytes exatos de todo port existente**; o generator só
  normaliza/emite o campo quando o port o declarou, e nada é ativado por
  autodetecção de Wayland, CFW ou nome de firmware.
- Declarado, o campo registra que o wrapper do port chama o par
  `pre_present`/`after_present` do adapter vendorizado (nxgl 0.3.1): a prova
  final `GRAPHICS-EVIDENCE` só nasce depois do primeiro present real do guest,
  com `phase=post-first-present` e `first_present=1` — que o nxrelease 0.3.1
  então exige no proof vinculado.
- Só é válido com `uses_gl: true`; valor desconhecido e uso em port não-GL
  falham fechados. Testes: round-trip do campo no `adapter-contract.json`,
  ausência nunca inventada, e negativos novos.

# 0.3.0 (2026-08-29, opt-ins declarativos da linha V4)

- Schema 3 ganha `display`, `graphics.egl_binding` e
  `controls.sdl3_portmaster`. Os três são **opt-in, desligados por omissão** e
  nenhum port existente muda de comportamento ao ser regerado.
- `display` (V4-DISPLAY-01): a ausência do bloco é `game` — o framework não
  instala viewport nenhuma. `preserve` faz letterbox e muda pixels, então é
  opt-in explícito e nunca default. Políticas finitas
  (`game`/`preserve`/`adaptive`/`fill`/`stretch`); limites só existem em
  `adaptive`; `adaptive` sem máximo declarado falha fechado. Nada é decidido
  por device, CFW, GPU ou nome de jogo.
- `graphics.egl_binding` (V4-GRAPHICS-03): quando ligado, exige o inventário
  exato de imports EGL do guest, ordenado lexicograficamente, sem duplicatas,
  só com nomes `egl*` e obrigatoriamente com `eglGetCurrentContext`. Ligar em
  port sem contrato gráfico `uses_gl` falha fechado; desligado com inventário
  não vazio também.
- `controls.sdl3_portmaster` (V4-CONTROLLERS-02): quando ligado, fixa o
  SHA-256 da SDL3 privada do port. Ligado sem pin, ou desligado com pin, falha
  fechado.
- Os três blocos são gravados sempre, já normalizados, no
  `adapter-contract.json`, para que a ausência no manifesto e a perda de um
  campo nunca se confundam.
- Novo gate `tests/test_v4_declarative.py`.
- O **ciclo HarbourMaster real** passa a rodar também sobre um port schema 3
  **com seed**. Os dois ciclos reais existentes usam exemplos schema 2, que não
  têm `generation_runtime` e portanto não têm `.nxb` nenhum: um port V4 é outra
  forma, com um arquivo visível ao lado do `nxport.json`, e autoinstall,
  discovery, uninstall e reinstall nunca o tinham visto. O ciclo aceita o port
  e o seed sai byte-intacto; **remover o seed do ZIP faz o ciclo real recusar o
  port**, então o seed é membro declarado do artefato e não um extra tolerado.

# nxgenerator changelog

## 0.2.20 — 2026-08-28 (nxbootstrap 0.6.37)

- Avança a composição canônica para o `nxbootstrap 0.6.37`, cuja geração v2
  autentica integralmente o store antes de restaurar modos `0644`/`0755`
  alterados por `chmod -R 777` do PortMaster em mídia POSIX.
- O recibo continua derivando versão e SHA-256 do gerador e do template do
  launcher; por isso a nova composição recebe `source_pins`, artefatos e
  `generation_id` próprios, sem reinterpretar gerações 0.2.19 já publicadas.
- Não muda schemas, NXExtract, NXSplash, ordem POSIX, interfaces visuais nem
  migra qualquer port automaticamente.

## 0.2.19 — 2026-08-28 (ordem canônica do inventário)

- Ordena `GENERATION.json.artifacts` pela string do path lógico POSIX, a mesma
  autoridade consumida pelo renderer e pelo validator, em vez da ordem por
  componentes de `pathlib.Path`. Assim, fronteiras de prefixo como
  `nxextract-version.txt` versus `nxextract/run-extractor.sh` permanecem
  estritamente lexicográficas e independentes do filesystem/host.
- Acrescenta regressão dirigida para os limites hífen, ponto, barra, dígito e
  caixa, além de conferir que modo e SHA-256 continuam ligados ao path correto.
  Nenhum payload, schema, launcher, UI ou fluxo visual muda.

## 0.2.18 — 2026-08-28 (composição autoral + tuning GPTK)

- Acrescenta ao nxproject schema 3 o array opt-in `package_payload`, com
  registros exatos `{path, mode, sha256, kind}`, ordem lexicográfica,
  unicidade casefold, até 128 arquivos/4 MiB por arquivo/16 MiB totais e cópia
  source-root-relative no mesmo path do port. `kind` fica fechado em
  `payload|license-notice` e `0755` só pode existir abaixo de `tools/`.
- Fecha a fronteira de conteúdo: fonte regular não-symlink de link único,
  modo/hash exatos e bytes retidos após uma única leitura no-follow. ELF,
  `.so`, containers de jogo e archives comuns, runtime/required_files,
  launcher/executável, membros gerados e namespaces de runtime/estado são
  recusados. `cover.png` também fica reservado ao renderer, que o classifica
  como `portmaster-image`, evitando dois produtores para o mesmo target. A
  materialização usa os bytes retidos, sem segunda leitura TOCTOU.
- Amplia `documentation.status` para `scaffold|authored`. `authored` exige
  `README.md` e `INSTALLATION.md` no payload e permite substituir somente esses
  dois membros, ambos com `kind: payload`; `license-notice` é recusado nesses
  paths. `scaffold` proíbe a substituição. A composição ocorre depois dos docs
  gerados e antes do inventário/recibo, que fixa os bytes finais.
- Acrescenta `controls.tuning.cursor` e `.camera` com chaves/ranges fechados,
  rejeição de desconhecidos, não finitos e bool-as-number, render determinístico
  sem expoente nas seções parser-valid `[cursor]`/`[camera]`. Os limites 0.05 e
  0.9 usam a forma decimal segura do mesmo float32 efetivo do parser C.
- Cursor tuning exige uma ação vetorial `cursor.*` ligada, mas não um contexto
  cursor dedicado: nesse caso o gerador anexa `[cursor]` somente com tuning e
  preserva menu/gameplay. Um contexto cursor explícito exige RIGHT_STICK
  vetorial e R3 botão, aceita nome semântico livre para R3 e continua proibindo
  que ações de cursor roubem A/D-pad.
- Novo gate registrado cobre composição/recibo/determinismo, retenção anti-
  TOCTOU, limites, symlink/hardlink, colisões e formatos proibidos; também
  compila o parser nxinput e valida o GPTK final. Schemas 1/2 e schema 3 sem os
  campos novos preservam o comportamento anterior; nenhum port migra sozinho.

## 0.2.17 — 2026-08-28 (schema3 + NXExtract real)

- Corrige a composição real de `runtime_root` com NXExtract: o nxbootstrap
  materializa primeiro a closure v2, e o nxgenerator agora reutiliza a pasta
  `nxextract/` existente em vez de tentar criá-la novamente.
- Recipe, engine, runner, runtime-env e UI preexistentes só são aceitos quando
  continuam arquivos regulares com os bytes e modos canônicos. Divergência
  falha antes da publicação; nenhum membro pinado é sobrescrito para esconder
  o erro.
- Acrescenta regressão positiva com NXExtract 1.2.21 completo e negativa em
  que o hash declarado acompanha uma engine não canônica. O caminho legado
  schema 2 continua materializando os mesmos arquivos como antes.

## 0.2.16 — 2026-08-28 (V3-UPDATE-01 generation-runtime integration)

- Integra o `nxbootstrap 0.6.36` e passa sua entrada obrigatória
  `runtime_root` ao gerar um `nxport` schema 3 com `generation_runtime`; a
  raiz é relativa ao `--source-root`, sem symlink e nunca vaza como caminho
  absoluto nos artefatos.
- Confere após a geração que a closure v2 está completa e idêntica na raiz
  ativa e em `.nxruntime/generations/<id>/`: papel, caminho, modo, SHA-256,
  manifesto, registros de identidade e commit precisam concordar com o
  `nxport.json` canônico.
- Mantém nxproject/nxport schemas 1/2 na geração de controle v1. `runtime_root`
  ausente em schema 3, presente fora dele, inexistente, atravessando symlink ou
  contendo membro com modo/hash divergente falha antes da publicação.
- Acrescenta gate dirigido com executável, biblioteca privada e hook reais,
  negativos de raiz/hash e regressão explícita de schema 2. Nenhum port é
  regenerado ou migrado por esta mudança.
- Emite `gameinfo.xml` mínimo, determinístico e canônico diretamente de
  `nxport.launcher_name`/`nxport.title`, valida o XML antes de publicar e fixa
  seus bytes 0644 no inventário de `GENERATION.json`; nenhum arquivo externo
  nem metadado editorial inventado entra no contrato.

## 0.2.15 — 2026-08-27 (V3-PROMOTION-01)

- Adiciona ao `nxproject` v3 a promoção explícita e opt-in de um adapter já
  implementado. Sem o bloco `promotion`, o resultado permanece byte-compatível
  com o scaffold não publicável e com todos os claims de release falsos.
- A promoção copia de uma fonte regular rastreável o contrato real do port;
  exige `implemented_release`, lifecycle implementado e coerência exata de
  idioma, ações/contextos GPTK e contrato gráfico. O gerador nunca inventa
  lifecycle, JNI, sinks ou evidência física.
- `release_ready`, `physical_support_proven` e
  `adapter_lifecycle_implemented` são declarações explícitas do port e seguem
  para `GENERATION.json`; a prova física continua sendo validada pelo
  `nxrelease` contra a geração e os receipts exatos.
- O schema, README, matriz e testes agora cobrem ausência de opt-in, promoção
  reproduzível e recusas de schema antigo, path inseguro, claim contraditório e
  contrato divergente.

## 0.2.14 — 2026-08-27 (V3-CONTROLLERS-01 contract closure)

- Schema-v3 agora exige `controls.actions` e `controls.contexts`: toda ação
  semântica possui tipo e pelo menos um sink real declarado pelo adapter;
  `menu` e `gameplay` são obrigatórios e ações desconhecidas, IDs/sinks
  duplicados, controles inexistentes e tipos incompatíveis falham fechado.
- `defaults/NEXTOSCONTROLLERS.gptk` de um port V3 é renderizado exclusivamente
  desse contrato. O template genérico `player.primary/player.secondary` fica
  restrito à compatibilidade byte-preservada dos schemas legados v1/v2 e não
  pode mais vazar para um port novo.
- Contexto opcional de cursor exige analógico direito + R3 em ações
  `cursor.*`, proíbe roubar A/D-pad e registra a autoridade física
  `sdl-portmaster-complete-first`; sticks aceitam somente ações vetoriais.
- O `adapter-contract.json` carrega ações com seus sinks, bindings por contexto
  e a autoridade do mapping. A fixture positiva prova o GPTK final, e os
  negativos cobrem silêncio, ação sem sink, ação desconhecida, duplicata,
  cursor inválido e vetor ligado a botão.
- Alinha o recibo com `nxbootstrap 0.6.32`: `generation_id` é o SHA-256
  determinístico completo de 64 hex, sem truncamento nem relógio.

## 0.2.13b — 2026-08-27 (V3-GRAPHICS-02 item 3)

- Bloco `graphics` opcional em nxproject schema_version 3 (schema + validador):
  `uses_gl` obrigatório; quando true, o contrato de CONTEXTO
  {api (gles|gl), profile (es|core|compat), version MAJOR.MINOR,
  version_policy (exact|minimum|range) + version_max só p/ range,
  shader_dialect (essl100|essl300|essl310|glsl-any),
  drawable_ready_timeout_ms, adopt_single_channel}. Os enums BATEM com
  `nxgl_graphics_contract.h` (schema e validador C nunca divergem). Coerência
  imposta: gles⇒profile es + dialeto ESSL; gl⇒core/compat + glsl-any;
  range exige version_max ≥ version; version_max proibido fora de range.
  `uses_gl:false` = porta sem GL, declara só isso. Nunca decide por
  device/CFW/nome do jogo — só registra o que a porta precisa para o adapter
  MEDIR o contexto obtido em runtime.
- O adapter-contract gerado passa a carregar o bloco `graphics` normalizado
  (com o adapter vendorizado `nxgl_graphics_contract_adapter` registrado), para
  o nxrelease exigir a prova gráfica quando a porta usa GL (item 5). Nada muda
  para portas sem o bloco (opcional; adoção opt-in).
- Campo opcional `graphics.required_devices` (lista única de devices que uma
  claim de release precisa provar): metadado de prova, nunca decisão de runtime;
  o nxrelease exige um `GRAPHICS-EVIDENCE` por device listado (cobertura).

## 0.2.13a — 2026-08-26 (auditoria V3, blocker 5)

- language_access obrigatório em nxproject schema_version 3: bloco
  {mode (native-menu|first-run-native|adapter|single-language|none),
  supported, fallback ∈ supported, sinks}. Silêncio no manifesto v3 é erro.
  O adapter-contract passa a carregar o bloco declarado (nunca null); o
  release gate (nxrelease) recusa language_access nulo ou fallback fora de
  supported numa geração. Testes cobrem os 5 modos + negativos.

## 0.2.13 — 2026-08-26 (V3: GAMEDATA-DIR-01 + V3-CONTROLLERS-01 scaffold + generation_id)

- GAMEDATA-DIR-01: every project with NXExtract enabled physically
  materializes `<port-id>/gamedata/README.txt` — a real regular 0644,
  non-empty, bilingual marker rendered from the canonical template
  `templates/GAMEDATA-README.txt.in` (never an empty ZIP directory entry).
  INSTALLATION.md now documents the exact `ports/<id>/gamedata/` path.
- New optional nxproject schema_version 3 block `owner_data`
  (`schema/nxproject-v3.schema.json`): single source for directory
  (canonical `gamedata`), accepted formats and reference version; the marker,
  the INSTALLATION text and the coherence gates all derive from it. A recipe
  whose `input.search_dirs` does not put `gamedata` first fails closed.
- V3-CONTROLLERS-01 (generator side): every new port ships the immutable
  default `defaults/NEXTOSCONTROLLERS.gptk`
  (`templates/NEXTOSCONTROLLERS.gptk.in`, format NEXTOS_CONTROLLERS/1 —
  NextOS own format, not gptokeyb).
- V3-UPDATE-01 groundwork: `GENERATION.json` gains a deterministic
  `generation_id` (hash of the artifact inventory) shared by the runtime.
- APK-COMPAT-01: recipe policy now runs on the shared canonical module
  contract (container identity banned in any quantity — see
  framework/contracts/apkcompat 1.0.0 and NXExtract 1.2.19).

## 0.2.12 — 2026-08-20

- Propaga `options` do nxproject para o `nxport.json` canônico e recusa, fail
  closed, um launcher gerado que exponha opções que o manifesto não declarou.

## 0.2.11 — 2026-08-20

- Adota por opt-in o `nxbootstrap 0.6.27` e o engine NXExtract 1.2.13.
- Quando `nxport.execution_roles` existe, seleciona o `nxextract-ui` pela ABI
  do papel extractor e o `nxsplash-nextos` pela ABI do papel splash; a ABI
  global continua sendo exclusivamente a ABI do jogo e do metadata PortMaster.
- Registra no `GENERATION.json` os papéis canônicos completos, incluindo
  executável, ABI, executor, `PT_INTERP` e closure. Os pins dos dois artefatos
  continuam trazendo ABI e SHA-256 próprios.
- Acrescenta um exemplo Spruce-class mixed ABI (extractor AArch64, splash/jogo
  ARMHF) e um gate de integração único. Exemplos legados permanecem sem
  `execution_roles`; projetos v1 não podem ativar silenciosamente o contrato.
- Mantém NXSplash 0.1.2 e NXExtract UI 1.2.9 byte a byte, sem alteração visual.

## 0.2.10 — 2026-08-16

- Adota por opt-in `nxbootstrap 0.6.15` e o engine NXExtract 1.2.10, incluindo
  resultado terminal e fronteiras de fase estruturados.
- Separa no recibo a versão do engine NXExtract da versão 1.2.9 da interface
  gráfica imutável, sem trocar renderer, pixels, ordem ou fallback visual.
- Acrescenta `--source-root` para ports standalone resolverem licença e receita
  por caminhos relativos sem depender do layout do monorepo nem vazar o caminho
  do host nos artefatos.
- Rejeita source root inexistente, symlink, travessia, input symlink e versão
  visual divergente antes de publicar qualquer árvore.

## 0.2.9 — 2026-08-16

- Adiciona `nxproject` v2 com `portmaster.runtime` obrigatório, único e sempre
  serializado como lista no `port.json`; projetos v1 continuam aceitos com o
  default seguro `[]` somente quando optam pela nova versão do componente.
- Fixa no recibo o contrato PortMaster v2, o schema suportado e o commit exato
  do HarbourMaster, e roda os exemplos gerados no parser/ciclo real offline.
- Gera `INSTALLATION.md` bilíngue obrigatório e explicitamente não publicável
  até o port preencher a identidade técnica dos dados legais do proprietário.
- Rejeita runtime ausente, nulo, escalar ou duplicado antes de publicar a
  árvore e mantém os ports existentes sem regeneração automática.
- Rejeita manifesto com BOM, chave duplicada, `NaN`, vírgula final ou JSON
  truncado; a serialização de saída continua UTF-8 canônica e determinística.

## 0.2.8 — 2026-08-15

- Adota por opt-in `nxbootstrap 0.6.14` e o kit NXExtract 1.2.9 completo,
  mantendo o launcher/core anterior e NXSplash 0.1.2 byte-idênticos.
- Aceita receitas APK sem hash do container quando package ID e âncoras internas
  fortes por SHA-256 ou estrutura provam o payload, e continua recusando um
  único SHA-256/CRC32 ou tamanho exato do APK externo.
- Preserva as receitas reais de Angry Birds, Retro Highway e ScourgeBringer,
  inclusive o container transformado de ScourgeBringer sem bounds/magic.
- Registra separadamente o recibo visual válido no Mali-450 e o recibo de
  renderer KMSDRM sem captura visual válida; nenhum deles declara jogo completo.

## 0.2.7 — 2026-08-15

- Select the NXExtract 1.2.8 UI from its immutable release manifest by the
  project architecture. ARMv7 can no longer receive an AArch64 ELF.
- Vendor only tracked release artifacts, never the ignored local
  `ui/build/nxextract-ui`, and pin manifest, source, architecture, mode and
  artifact SHA-256 in `GENERATION.json`.
- Adopt nxbootstrap 0.6.13 and NXSplash 0.1.2 while their preservation gates
  keep every 0.6.8 launcher fix and both validated graphical interfaces
  unchanged. This remains opt-in and does not regenerate existing ports.
- Reject cross-architecture, hash, source, toolchain and GLIBC tampering before
  publication, and keep the generated public shell path free of external
  `stat` calls.

## 0.2.6 — 2026-08-14

- Adota nxbootstrap 0.6.12 e o kit NXExtract 1.2.7 completo nos scaffolds que
  optam pela fase BYO-data.
- Fixa os novos hashes do core, runner e UI GLIBC 2.17 no recibo de geração; a
  UI agora precisa atestar renderer visível e permanecer viva durante o setup.
- Mantém a validação flexível de APK por package/ABI e conteúdo crítico, sem
  prender compatibilidade a um único SHA externo, e não migra ports antigos.

## 0.2.5 — 2026-08-14

- Adota nxbootstrap 0.6.11 e inclui automaticamente o `nxextract-ui` canônico
  em modo 0755 em todo scaffold com NXExtract ativo.
- Fixa o SHA-256 da interface no `GENERATION.json` e no inventário, rejeitando
  ausência, troca, symlink ou modo divergente antes de qualquer release.
- Rejeita receitas de APK-container presas a um único SHA/tamanho externo:
  exige package ID, faixa de tamanho, magic ZIP e identidade por conteúdo
  crítico ou por pelo menos duas variantes explícitas.

## 0.2.4 — 2026-08-14

- Adota nxbootstrap 0.6.10 e nxsplash 0.1.1 nos scaffolds e recibos novos.
- Fixa os novos bytes por arquitetura e a identidade de fonte/manifesto do
  fallback de VT ativo, mantendo a ordem do handoff e a ausência de opção de
  remoção.
- Preserva o opt-in: nenhum port já aprovado é regenerado automaticamente.

## 0.2.3 — 2026-08-14

- Adota o contrato do nxbootstrap 0.6.9 e inclui automaticamente o
  `nxsplash-nextos` 0.1.0 da arquitetura escolhida em todo scaffold novo.
- Fixa no `GENERATION.json` versão, manifesto de release, fonte e SHA-256 do
  helper, além de inventariar seus bytes e modo 0755.
- Rejeita helper ausente, trocado, com modo divergente ou invocação movida para
  antes do gate de payload/depois do adapter e bibliotecas privadas.
- Acrescenta `nxsplash` ao registry e schema do `FRAMEWORK-BUILD-PIN.json`, para
  que builds consumam a mesma árvore imutável que gerou o helper.
- Mantém os exemplos ARMv7/AArch64 sem claims físicos e sem migrar ports
  existentes automaticamente.

## 0.2.2 — 2026-08-13

- Adiciona `framework_pin.py`, um gate pré-build separado que cria o schema
  `nextos-framework-build-pin-v1`, materializa somente objetos Git apontados
  por commits completos e reaudita a árvore antes de publicá-la.
- Define o digest `nxgenerator-component-tree-sha256-v1` sobre modo Git,
  caminho UTF-8 relativo, tamanho e SHA-256 de cada blob em ordem binária.
- Rejeita commit abreviado no pin, componente desconhecido, versão divergente,
  digest alterado, symlink, gitlink, propriedades extras, JSON não canônico e
  destino já existente. Replace refs e variáveis `GIT_*` herdadas não podem
  redirecionar os objetos pinados.
- Recalcula a identidade SHA-1 dos objetos commit/tree/blob, bloqueia lazy fetch e
  clean/smudge filters, ignora executável Git injetado por `PATH`, impõe
  timeout/limites, exige `VERSION` canônico e detecta
  hardlink, diretório Git vazio, mtime adulterado em nanossegundos e ancestrais
  symlink.
- Normaliza modos e mtimes do snapshot, publica por
  `renameat2(RENAME_NOREPLACE)` (inclusive o pin, após gravação temporária) e grava
  `FRAMEWORK-SOURCE.json` sem versão autoatestada para que o build consuma somente
  `<snapshot>/framework` em modo somente leitura.
- Mantém `nextos-framework-release-pin-v1` histórico fora desse contrato: seus
  hashes não possuíam algoritmo reprodutível definido e não são
  reinterpretados retroativamente. NXExtract continua no pin próprio do
  `GENERATION.json`.

## 0.2.1 — 2026-08-13

- Alinha o gate de deployment à ordem canônica do `nxbootstrap 0.6.8`:
  `BIN`, handoff do PortMaster, NXExtract, arquivos obrigatórios e launch.
- Corrige a regressão em que o gerador 0.2.0 recusava os próprios exemplos
  oficiais apesar de o launcher renderizado ser byte-idêntico ao canônico.
- Adiciona adulteração negativa que põe o gate de arquivos antes do NXExtract
  e comprova rejeição fechada.

## 0.2.0

- Fecha o scaffold determinístico M19 para ARMv7 e AArch64, com publicação
  no-replace, launcher canônico e NXExtract pinado.

# 0.3.8 (2026-08-30, controles editáveis viram RUNTIME opt-in)

- `controls.runtime_mapping` (único valor aceito: `"nxinput-gptk"`) declara
  que o NEXTOSCONTROLLERS.gptk é carregado pelo executável do port através do
  runtime canônico do nxinput (loader owner/default, decisão tri-state,
  dispatcher com contextos e sinks reais do engine). O campo é opcional e a
  ausência não muda um único byte gerado — ports aprovados regeneram
  idênticos (test_c4_gptk_bytes). Quando declarado, o adapter-contract ganha
  `input.runtime_mapping` e o nxrelease 0.3.10 passa a exigir o marcador
  `nxinput-gptk-runtime/1` no executável empacotado.
- Origem: TEARSCAPE-CONTROLS-LIVE — o arquivo prometia "This file is YOURS"
  sem nenhum código carregá-lo. A promessa de edição agora é um contrato
  verificado, nunca documentação.
