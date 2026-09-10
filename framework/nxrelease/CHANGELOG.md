# 0.4.11 (2026-09-05, composição: nxbootstrap 0.8.4)

- Exige exatamente nxbootstrap 0.8.4, que remove do filho somente o preload
  SDL1/2 de sistema herdado, obsoleto e não mapeado observado fisicamente no
  ArkOS, mantendo os demais overrides não resolvidos em recusa fechada.
- O stage/ZIP recusa nxbootstrap 0.8.3 e qualquer identidade anterior. A
  correção de ambiente não cria exceção para SDL privada: os gates de nome,
  ELF, SONAME, símbolos e proveniência do pacote permanecem inalterados.
- nxgenerator permanece 0.4.5 e continua derivando versão e SHA-256 dos
  sources canônicos do bootstrap para cada geração.

# 0.4.10 (2026-09-05, composição: nxbootstrap 0.8.3)

- Exige exatamente nxbootstrap 0.8.3, que admite o token `$LIB`/`${LIB}` do
  `LD_PRELOAD` herdado somente com prova da expansão já carregada pelo dynamic
  loader. O NXRelease continua recusando qualquer SDL1/SDL2 empacotada.
- nxgenerator permanece 0.4.5: ele deriva versão e SHA-256 dos sources reais
  do nxbootstrap e registra os novos bytes no `GENERATION.json`.
- O fluxo humano, a única reabertura do ZIP e todos os demais gates de release
  permanecem inalterados.

# 0.4.9 (2026-09-04, composição: nxgenerator 0.4.5)

- Exige exatamente nxgenerator 0.4.5, que acrescenta o contrato aditivo
  `video.auto_algorithm=stretch`. O bundle humano continua com uma única reabertura do ZIP;
  nenhum gate ou autoridade de release foi alterado.

# 0.4.8 (2026-09-04, V5: autoridade humana e uma abertura do ZIP)

- O fluxo novo sem `--candidate-lock` usa `release_authority=human`: o dono aprova o
  comportamento e o framework valida somente fatos mecânicos do artefato. Nenhum receipt de
  device é inventado ou exigido para `stage`, `build` ou `bundle`. O modo histórico
  `candidate-lock` permanece disponível ao fornecer o lock ou selecioná-lo explicitamente.
- `bundle` reabre o ZIP final exatamente uma vez. Foram removidas as verificações idênticas do
  ZIP temporário, do diretório antes do rename e do helper gerado; integridade, inventário,
  SBOM, PortMaster, GLIBC, privacidade e hash continuam fechados nessa única abertura.
- O template canônico seleciona `--authority human`, e `nx-ship-port` não reabre novamente um
  pacote schema 3 que acabou de passar pela pipeline canônica. Legacy e `--skip-build` ainda são
  verificados porque sua origem não garante essa fronteira.

# 0.4.7 (2026-09-04, composição: nxgenerator 0.4.4)

- Exige exatamente nxgenerator 0.4.4 (canal do gatilho decidido por todos os contextos — Nameless Cat
  1.2.8: R2 = cursor.click só no menu, null em gameplay). Nenhum gate muda.

# 0.4.6 (2026-09-04, ordem do NextOS: aprovação humana ao vivo é uma classe de evidência)

- `HUMAN_LIVE_APPROVAL`: o dono do projeto, presente, validou o ELF exato no aparelho (controles, prompts,
  owner-remap). `nx-input-proof-lock.py --human-live-approval 'quem|aparelho|o que' --device-log log.txt` gera
  o lock a partir do LOG REAL da execução aprovada (promoção NXU0006 saudável, prova de vídeo OK, saída pelo
  SELECT+START), com o bloco `approval` (quem, quando, aparelho, validado, sha256 do log). ELF, adapter-contract,
  defaults e geração continuam presos por hash. Nunca se apresenta como `ON_DEVICE_AUTOMATED_INPUT_PROOF`.
- `stage`/`bundle` aceitam a classe humana em ports com `controls.proof`; a classe automática continua a norma
  quando o dono não está presente.

# 0.4.5 (2026-09-04, composição V5): exige nxgenerator 0.4.3

- `NXGENERATOR_REQUIRED_VERSION` = 0.4.3 (START isolado antes dos sticks nos roteiros). Nenhum gate muda.

# 0.4.4 (2026-09-03, V5: composição com nxgenerator 0.4.2 / nxinput 0.11.6)

- `NXGENERATOR_REQUIRED_VERSION` = 0.4.2 (`controls.proof.effects` → `expect: context_change` nos roteiros;
  nxinput 0.11.6 no oráculo). Nenhum gate muda de semântica; só a identidade exata exigida do gerador.

# 0.4.3 (2026-09-03, V5 revisão 2: vendor contra o COMMIT pinado, predicado de init, closure não vazia)

- F5: `validate_vendor_pin_contract` compara cada arquivo vendorizado com a ÁRVORE DO COMMIT que o FRAMEWORK-PIN.json
  declara (git ls-tree/cat-file), nunca com o checkout que roda o gate; cobre cópias soltas fora de `vendor/`
  (layout `src/nxinput/` do Tearscape) por nome de fonte; sem FRAMEWORK-PIN.json ou commit desconhecido = reprova.
  `vendor-drift-v1.json` remedido contra o commit pinado (FP2 zerado; NC/Blossom carregam a dívida até a M3).
- F7: `validate_port_init_predicate` — fonte do port com `SDL_WasInit(0)`/`SDL_INIT_EVERYTHING` reprova (a fronteira
  de staging é da costura; o port chama `nxc6_stage_before_init`). Gate `nxrelease-port-init-predicate`.
- F9: `CONTROLS-CLOSURE.json` com `cases` vazio ou contexto declarado sem caso reprova.
- Exige nxbootstrap 0.8.2.

# 0.4.2 (2026-09-03, V5: closure de controles e prova de prompt no `validate`)

- `vendor-drift-v1.json` remedido depois da integração: o Blossom passou de 4
  para 8 arquivos porque o nxcompat 0.5.1 (autoridade de aspect +
  `invalid_policy`) deixou o vendor dele para trás em
  `nxcompat_video.{h,c}` e `nxcompat_settings.{h,c}`. É o gate funcionando:
  a dívida cresce à vista e o M3 tem de revendorizar antes de qualquer
  alegação de ELF byte-idêntico. Totais medidos: FP2 4, Nameless Cat 6,
  Blossom 8.

- **`validate_controls_closure` (M1c 1.2/1.3).** O harness de host do
  Tearscape derivava a expectativa DENTRO do port e ficou no loader V1–V3
  quando o owner virou schema 4: a prova toda foi a vermelho em `NXI1006` e o
  laço da rodada 3 morreu lendo o rabo da cascata (E11 da auditoria). Pela
  regra 8.1 o esperado nasce do projeto — o nxgenerator 0.4.1 emite
  `CONTROLS-CLOSURE.json` — e o `validate` passa a cobrar que ele exista para
  todo port de controles schema 4, seja `nx-controls-closure/1`, nomeie o port
  certo, declare contextos e traga só casos de UMA entrega `ACTION` em evento
  press/axis/motion, todos em contexto declarado. Um caso num contexto que o
  port nunca declarou reprova: é exatamente a confusão que fez a closure do
  harness do Tearscape exigir `CURSOR`, contexto inalcançável naquele port.
- **`validate_prompt_capture_proof` (M1c 2).** O esquema
  `nx-prompt-capture-proof/1` aprovou `PRESS [10] TO BEGIN` no FP2 com
  `result: PASS` e `forbidden: []` (achado E13): o regex conhecia
  `Joystick Button N`, `Button N` e `Axis ±N`, mas o jogo desenha o PRÓPRIO
  ícone com o ordinal cru dentro e o OCR de tela inteira nem enxerga o dígito.
  Um esquema capaz de aprovar exatamente o defeito que deveria pegar não é
  prova: o `/1` é recusado por IDENTIDADE. Em `/2` o receipt tem de trazer
  regiões declaradas (OCR isolado), e um `PASS` exige pelo menos um glyph
  ESPERADO reconhecido — sem expectativa o veredito é `INCONCLUSIVE`, nunca
  `PASS` por silêncio — e qualquer `raw_ordinal_tokens` numa região derruba o
  `PASS`. `INCONCLUSIVE` continua veredito legítimo: o gate recusa PASS falso,
  não incerteza honesta.
- Ferramenta `framework/tools/nx-prompt-capture-proof.py` (`/2`) entra na
  árvore para que M2/M3 rodem a prova sem reinventá-la; a `/1` deixa de valer.
- Gates novos `tests/test_prompt_capture_gate.py` (10 checks) e
  `tests/test_controls_closure_gate.py` (9 checks), cada um com o mutante
  central e, no primeiro, o controle positivo: o receipt `/1` que está hoje em
  `ports/fp2/proofs/` TEM de ser recusado.

# 0.4.1 (2026-09-03, V5: gate de VENDOR BYTE A BYTE)

- **Defeito fechado (auditoria de 03/09, item E3).** Um port que compila
  `vendor/<componente>/` embute uma CÓPIA do framework, e o único conferidor
  era o próprio `PINS.json` — recalculado junto com a cópia, portanto sempre
  coerente consigo mesmo. Um vendor VELHO passava sem uma linha de aviso. Foi
  o que aconteceu: FP2, Nameless Cat e Blossom declararam pin no commit
  congelado do framework carregando `nxinput/src/nxinput_provider.c` de ANTES
  do provider estático (217 linhas contra 240), e o "ELF byte-idêntico"
  comemorado era consequência de NÃO revendorizar. O pin mentia.
- `validate_vendor_pin_contract`: para cada `vendor/<componente>/PINS.json`
  do port, cada arquivo vendorizado é comparado BYTE A BYTE com
  `framework/<componente>/<caminho>` — a árvore é a verdade, não o arquivo de
  pin. Qualquer diferença reprova nomeando o arquivo e citando os dois commits
  (o do `PINS.json` e o do `FRAMEWORK-PIN.json`). Também reprovam: arquivo que
  contradiz a própria entrada do `PINS.json`, arquivo presente e não declarado,
  conjunto de pins vazio, componente vendorizado ausente do `FRAMEWORK-PIN.json`
  e — a rota de fuga óbvia — apagar o `PINS.json` de um diretório cujo nome é
  de um componente do framework. Uma árvore de TERCEIROS (`vendor/sdl2`,
  `vendor/build_fdk.sh`) não precisa de pin de componente.
- Fronteira: `vendor/` é entrada de build, nunca é staged nem empacotado, então
  o contrato é de FONTE. `verify_stage`/`verify_archive` auditam um conjunto de
  registros sem árvore de fonte e não carregam `source_root`; quem cobra são
  `validate` e `build` (que valida as fontes antes).
- `vendor-drift-v1.json` (`nxrelease-vendor-drift/1`) registra o estado MEDIDO
  do vendor dos pilotos no dia em que o gate nasceu — 4 arquivos de `nxinput`
  em FP2/NC/Blossom e mais 2 de `nxgl` no Nameless Cat (o
  `nxgl_frame_proof_adapter` vendorizado não tem
  `nxgl_frame_proof_set_video_size`), achado novo desta frente. O gate mede a
  divergência viva e ela precisa ser IGUAL ao registro: divergência que cresce
  sozinha reprova, e zerar uma entrada exige revendorizar o port e esvaziar a
  entrada no MESMO commit. A dívida é visível e só sai por ato deliberado.
- Gate novo `tests/test_vendor_pin_gate.py` (14 checks) com o mutante central:
  um vendor adulterado cujo `PINS.json` foi recalculado para bater com ele
  ainda reprova.

# 0.4.0 (2026-09-03, V5 FV6/7A: gates de owner runtime e NEXTOS_SETTINGS/2)
- `NEXTOS_CONTROLLERS/4` (schema 4): `_validate_gptk4_closure` — header
  (`CONTROL_STANDARD=xbox`, `AUTHORITY` nextos|synchronized), `[base]` completo
  (omissão nunca é null oculto), overrides esparsos (cópia integral do base
  recusada), sticks/triggers com modos coerentes, bindings tipados cujas ações o
  adapter contract declara com sink, nenhuma seção V3 nem `FACE_LAYOUT`. O
  marcador vivo esperado passa a seguir o schema do default empacotado
  (`nxinput-gptk-runtime/4`), e um port /4 exige `NXC6-PROVIDER` no ELF.
  Gate novo `nxrelease-gptk4` (positivo = default FP2 gerado; 10 mutantes).

- `validate_owner_runtime_contract` (source, stage e ZIP): sob
  `nxport.owner_runtime="1"` (nxbootstrap 0.8.0) exige `defaults/port-env.sh`
  como payload 0644 (SEMENTE), recusa o `<port>/port-env.sh` vivo no pacote,
  recusa `port-env.sh` como membro de geração (seria curado) ou required file,
  exige `adapter-env.sh` como `runtime-hook` selado da geração, exige
  `OWNERSHIP.json` (`nx-ownership/1`, nxgenerator 0.4.0) coerente
  (owner-seeded → live nunca empacotado, sealed-runtime na closure,
  owner-native fora da closure, `healed=false` nos owners) e aplica a lição
  KOTOR à semente e ao helper selado (todo `$GAMEDIR/<arquivo>` referenciado
  está no stage). Em qualquer pacote: nenhum `*.new` e nenhum owner tipado
  vivo (`NEXTOSSETTINGS.txt`/`NEXTOSCONTROLLERS.gptk`) fora de `defaults/`.
  Ports V4 sem o opt-in: comportamento inalterado.
- `defaults/NEXTOSSETTINGS.txt`: aceita `# NEXTOS_SETTINGS/2` (chaves
  `video.*` com os MESMOS enums do nxcompat 0.5.0, espelhados em
  `settings_video_value_ok`; `video.*` sob `/1` falha fechado). Com
  `nxproject.video` declarado, o seed tem de ser `/2` com TODAS as chaves de
  vídeo explícitas e iguais aos defaults declarados (aspect entre as policies
  que o port implementa); sem `video`, chave de vídeo no seed falha.
- Gate novo `nxrelease-owner-runtime` (puro; mutantes: owner vivo empacotado,
  hook na closure, semente ausente, `.new`, helper não selado, native config
  curado, manifesto ausente, settings vivo empacotado, `healed` mentindo).
- A composição V5 repina o gerador/template do nxbootstrap 0.8.0
  (`GENERATION_V2_BOOTSTRAP_VERSION`) na branch de integração.

# 0.3.26 (2026-09-02, receipt ON_DEVICE_AUTOMATED_INPUT_PROOF no candidate-lock)

- `input_proof.evidence_class` (opcional) no candidate-lock externo: só
  `ON_DEVICE_AUTOMATED_INPUT_PROOF` é aceito; qualquer outra classe
  (`HOST_FIXTURE` inclusive) falha fechado. Um port que declara
  `controls.proof` no nxproject (nxgenerator 0.3.16) EXIGE essa classe: a
  prova de controles vem do framework no aparelho real (clones uinput
  device-faithful, nxinput 0.10.2), sem testemunha humana como requisito.
- `nx-input-proof-lock.py`: converte os receipts `nx-device-input-proof/1`
  (uma ou mais execuções default do MESMO ELF/mapping, all_pass) no lock
  externo canônico — casos por contexto/controle/ação/sink a partir do readback
  `nxinput-gptk-event-evidence/1`, vídeo/generation/run do log da execução,
  hashes do ELF, do adapter e do default conferidos contra o receipt. Recusa
  receipt sem a classe, com veredito FAIL, ou cujo ELF/adapter/default não
  batem com o candidato.
- Locks antigos (sem o campo) continuam válidos para ports sem
  `controls.proof`: ports aprovados não migram automaticamente.
- Composição: `GENERATION.json` passa a vir do nxgenerator 0.3.16 (o gerador
  dos roteiros da prova automática); pinos dos gates atualizados.

# 0.3.25 (2026-09-01, adaptador estrutural: nxscan LIGADO ao fluxo real)

- `nxscan.py` deixa de ser fundacao isolada. `nxrelease.py` carrega o modulo
  irmao pelo mesmo padrao do `apkcompat` (`spec_from_file_location`), valida
  o schema `nx-structural-scan/1` na carga e falha fechado se o scanner
  sumir ou mudar de contrato.
- `_structural_secret_verdict()` roda ANTES do fallback textual para os
  tipos que o scanner entende de verdade (`.py`, `.pyi`, `.json`): a
  gramatica decide por AST/JSON estrito em vez de por regex de bytes.
- O adaptador e ESTRITAMENTE ADITIVO. `FAIL` estrutural rejeita na hora
  (literal Python entre aspas, literal `bytes`, credencial aninhada em dict
  ou em subarvore JSON — tudo invisivel para a regex). `PASS` NUNCA absolve:
  a autoridade regex historica e deliberadamente mais ampla (tambem recusa
  um *nome* ligado a identificador sensivel) e continua decidindo.
- `UNSUPPORTED` (tipo nao parseado) e `STRUCTURAL_ERROR` (sintaxe invalida,
  BOM, chave duplicada, NaN, nesting excessivo, tamanho acima do teto)
  retornam ao fallback fail-closed da regex — nenhum deles vira PASS.
- Excecao do scanner degrada para a regex, jamais para aprovacao.
- O scanner de nomes privados, caminhos de host, IPv4 e advocacy fica
  intocado.
- `tests/test_nxscan.py`: o negativo `test_unwired` vira regressao positiva
  de wiring (`WiringTest`, 10 casos) — modulo carregado, FAIL estrutural
  chegando ao scanner vivo, PASS nunca absolvendo, aditividade provada
  contra os 4 shapes que so a regex pega, o falso positivo de anotacao
  Python continuando ausente, e crash do scanner degradando para a regex.
  O corpus adversarial (sintaxe invalida, BOM, duplicata, NaN, nesting,
  segredo sem eco) permanece integral.
- Versao/contrato/README/matriz/runner e pins acompanham; nenhuma outra
  fronteira muda de comportamento.

# 0.3.24 (2026-09-01, contrato de release GPTK V3 / FACE_LAYOUT — nxinput 0.10.0)

- Aceita defaults `NEXTOS_CONTROLLERS/3` com exatamente UMA linha
  `FACE_LAYOUT = auto|modern|retro` (minusculas exatas) no preambulo;
  ausencia, duplicata, case errado ou valor invalido falham fechado; V1/V2
  nao podem carregar o campo V3.
- `input_controller_profiles` ganha `face_layout_variants` (somente sob
  default V3; um port V3 com profiles habilitado EXIGE o par completo):
  nomes fixos `controllers-{modern,retro}.nxb`, um SHA-256 por variante,
  pins distintos, closure por variante (empacotada, byte-exata, header V1,
  license, sem dado pessoal, sem symlink, sem vazio, teto 8 MiB); o par tem
  o mesmo conjunto de identidades e diverge SOMENTE nos bindings a/b/x/y +
  metadata `#`; GUID mutavel do par jamais reentra na base invariante
  `controllers.nxb` (o defeito de campo do 0.9.0).
- Marcador de runtime vivo passa a `nxinput-gptk-runtime/3` (schema do
  candidate-lock e nx-render-manifest acompanham; oraculo N28: runtime
  V3-capaz nunca reusa /2). Um port V3 exige no ELF a identidade da costura
  0.10.0 (linha `NXC6-DOMAIN` + path canonico do banco vivo) e NENHUM
  executavel pode carregar o fallback generico em quarentena
  (`nx_add_generic_gamepad_mappings`/"Generic Xbox Fallback").
- Composicao pinada: nxinput 0.10.0, nxgenerator 0.3.15
  (`NXGENERATOR_REQUIRED_VERSION`), nxobs 0.4.4, nxledger 0.2.3; contrato
  declarativo 1.0.67.
- Gate novo tests/test_face_layout_release.py (trio valido + 11 negativos),
  integrado ao test_nxrelease.sh e a matriz.

# 0.3.23 (2026-09-01, preflight agregado somente leitura — V4-05A)

- Compoe 03B (nxabi 0.2.2->0.2.3 + nxrelease 0.3.22) sobre a linha 04A
  (nxledger 0.2.0), preservando os dois lados: nxinput 0.9.0 na composicao
  corrente E a politica unica de piso SDL.
- Novo comando `nxrelease preflight` (modulo nxpreflight.py): fronteira
  agregada SOMENTE LEITURA que avalia todas as categorias de release em
  ordem deterministica, reporta TODOS os erros independentes (dependencia
  reprovada vira BLOCKED, nunca PASS/SKIP), nao cria stage/ZIP/copia/hook,
  e emite exatamente o receipt `org.nextos.v4.preflight-receipt/1` que o
  nxledger aceita, vinculado a commit/tree/perfil/manifesto de inputs/
  autoridade SDL/identidade das tres ferramentas. `--out` so aceita arquivo
  NOVO, atomico, externo ou provadamente git-ignored.
- `public-final` exige `--preflight-receipt`: PASS, perfil PUBLIC-FINAL e
  commit/tree exatos ANTES da primeira mutacao; receipt DEV, FAIL, stale ou
  malformado recusa nomeando o motivo (um receipt DEV jamais autoriza
  candidato publico).
- Paridade integral do piso SDL (correcao da auditoria 03B): a decisao vem
  de `nxabi.decide_sdl_floor()` nos DOIS consumidores; simbolo ausente da
  autoridade falha fechado nos dois para candidato publico; o piso alcanca
  consumidores INDIRETOS de SDL (sem DT_NEEDED direto do core), preservando
  a exclusao SDL3 documentada; waiver historico nunca rebaixa veredito
  publico nem readmite Vendor/Product acima de 2.0.4.
- Gates novos: tests/test_preflight.py (read-only provado por snapshot,
  receipt aceito pelo loader real do nxledger, staleness por commit/tree,
  disciplina do --out) e tests/test_sdl_floor_parity.py (7 classes),
  integrados ao test_nxrelease.sh.

# 0.3.22 (2026-08-31, política única de piso SDL — V4-03B)

- O erro real exposto pelo FP2 morre aqui: a closure aceitava
  `SDL_JoystickGetVendor`/`SDL_JoystickGetProduct` porque a lista paralela
  `symbol-floors/libSDL2-2.0.so.0.syms` (interseção de firmwares reais, mais
  novos que o piso) os continha. A lista paralela do core da SDL2 foi
  REMOVIDA; se um arquivo desses reaparecer em `symbol-floors/`, o load
  falha fechado.
- A decisão para todo import direto `SDL_*` de ELF que declara NEEDED da
  SDL2 core passa a ser por VERSÃO DE NASCIMENTO, lida da autoridade única
  `framework/nxabi/sdl2-symbol-floor.tsv` através do próprio parser estrito
  do nxabi (`load_sdl_authority_for_policy`) — os dois consumidores abrem os
  mesmos bytes pela mesma rota. Import acima do piso universal SDL 2.0.4
  reprova nomeando ELF, símbolo, versão exigida, piso declarado e o SHA-256
  da autoridade; símbolo fora da autoridade também reprova (não prova o
  piso). Este gate não tem waiver e roda em `validate_sources`, a primeira
  fronteira somente leitura — antes de stage, cópia, build ou ZIP.
- `assert_abi_policy_agrees()` também trava `sdl.floor == 2.0.4` (espelho
  imutável, mesmo padrão dos tetos glibc); o recibo do `validate` ganha
  `sdl_floor`, `sdl_authority` e `sdl_authority_sha256`.
- A rota canônica para APIs pós-piso continua o resolver opcional do
  nxcompat 0.4.0 (sem import direto): ausência numa SDL 2.0.4 é segura e a
  SDL mais nova entrega o dado. ELF que EMPACOTA a SDL2 core segue fora do
  gate de piso, como antes; as demais famílias (`.syms` de mixer, image,
  ttf, freetype, openal, zlib) permanecem inalteradas.
- A 0.3.20 e a 0.3.21 entram na autoridade histórica somente leitura. Não há
  tag, release, merge, ZIP nem migração automática de port; a composição da
  V4 (03A) é ato separado.

# 0.3.21 (2026-08-31, composição nxinput 0.9.0)

- Fixa a composição corrente no nxinput 0.9.0: a normalização de domínio
  PortMaster/joydev comprovada por capabilities entra na fronteira comum de
  autoridade C6, sem seleção por CFW, nome, placa ou VID/PID.
- Nenhum gate muda de semântica: bundle pinado da autoridade 3, GPTK vivo,
  marcadores Godot, scanner de privacidade 0.3.19 e `validate` opcional
  0.3.20 permanecem literais.
- Preserva integralmente nxbootstrap 0.7.8, nxgenerator 0.3.12, nxgl 0.3.4 e
  todos os demais gates; não altera interface visual, geração nem bytes de
  port.
- A 0.3.20 entra na autoridade histórica externa somente leitura. Não há tag,
  release, merge, ZIP nem migração automática de port.

# 0.3.20 (2026-08-31, validate opcional na receita NXExtract)

- Alinha o gate de receita à semântica do NXExtract 1.3.0: ausência do campo
  top-level `validate` equivale a `validate: []`.
- `extract` e `commit` continuam arrays obrigatórios; um `validate` presente
  que não seja array continua falhando fechado.
- Preserva integralmente o scanner de privacidade corrigido na 0.3.19,
  nxbootstrap 0.7.8, nxgenerator 0.3.12 e todos os demais gates.
- A 0.3.19 entra na autoridade histórica externa somente leitura. Não há tag,
  release, merge, ZIP nem migração automática de port.

# 0.3.19 (2026-08-31, scanner de segredos em Python anotado)

- Distingue a anotação de tipo Python `name: Type` de um literal no formato
  `key: value`, evitando que fontes geradas legítimas como
  `m_VCPassword: Optional[str] = None` sejam recusadas.
- Mantém fail-closed atribuições reais sem anotação, como
  `password=abcdefgh`, e passa a cobrir explicitamente também atribuições
  anotadas cujo valor é um literal com aparência de credencial.
- Preserva integralmente nxbootstrap 0.7.8, nxgenerator 0.3.12 e todos os
  demais gates; não altera interface visual, geração nem bytes de port.
- A 0.3.18 entra na autoridade histórica externa somente leitura. Não há tag,
  release, merge, ZIP nem migração automática de port.

# 0.3.18 (2026-08-31, composição nxgenerator 0.3.12)

- Fixa a composição corrente em nxbootstrap 0.7.8 e nxgenerator 0.3.12.
- Adota a distinção explícita do gerador entre o mapa nativo estático, que
  avisa que edições não chegam à engine, e o mapa vivo declarado por
  `controls.runtime_mapping = "nxinput-gptk"`, que preserva o cabeçalho
  editável anterior e a prova completa de runtime.
- Mantém fail-closed a fronteira já existente: um default que prometa edição
  sem runtime declarado continua inelegível; nenhum port aprovado migra.
- A 0.3.17 entra na autoridade histórica externa somente leitura. Não há tag,
  release, merge, ZIP, mudança visual nem migração automática de port.

# 0.3.17 (2026-08-31, provenance fechada da closure de hooks)

- Remove as duas exceções globais que transformavam metadata em autoridade
  sobre código: um digest declarado em `patch_profiles` ou sob nomes como
  `output_sha256`/`sha_out` não pode mais reaparecer na closure executável.
- Preserva a autenticação de `patch_profiles` pela receita e o fallback
  genérico, mas o hook passa a consumir somente o id/ambiente do perfil já
  selecionado. Integridade pós-transformação fica em `output_validate`,
  checkpoints canônicos da receita ou `generation_runtime`.
- Fecha o bypass por extensão: um record alcançável com sufixo desconhecido é
  incluído quando seus bytes pequenos são UTF-8 estrito e formam objeto/array
  JSON; suas referências string são percorridas como em `.json`.
- ELF declarado, magic ELF, NUL, não-UTF-8, conteúdo grande, não-JSON e JSON
  escalar não são enfileirados nem tratados como fonte. O gate dirigido prova
  paridade `.json`/`.blob`, maiúsculas, profundidade transitiva e fallbacks.
- Mantém nxbootstrap 0.7.8 e nxgenerator 0.3.11. A 0.3.16 entra na autoridade
  histórica externa somente leitura; não há tag, release, merge, ZIP, mudança
  visual ou migração automática de port.

# 0.3.16 (2026-08-31, biblioteca privada do NXExtract)

- Fixa a composição corrente em nxbootstrap 0.7.8 e preserva nxgenerator
  0.3.11, cuja leitura dinâmica de `framework/nxbootstrap/VERSION` reseala os
  sources sem mudança de lógica própria.
- Um ELF autenticado como `private-library` sob `nxextract/` continua usando a
  classificação já existente `third-party-linux`; modo `0644`, SHA-256 e
  metadados Linux são preservados no live e no store imutável.
- Fecha o `public-final` sobre a mesma árvore: essa private library passa a
  integrar o conjunto NXExtract esperado, evitando que um candidato gerado
  corretamente seja recusado como arquivo vivo extra. Nenhum papel novo foi
  criado; `nxextract-helper` ELF segue `project-linux` e `0755`.
- A 0.3.15 entra na autoridade histórica externa somente leitura. Não há tag,
  release, merge, migração automática de port ou mudança visual.

# 0.3.15 (2026-08-31, helpers ELF e recibo GPTK por opt-in)

- Classifica um `nxextract-helper` ELF autenticado pela `generation_runtime`
  como `project-linux`, exigindo modo `0755` e submetendo o helper à mesma
  auditoria Linux, teto GLIBC e reconstrução limpa dos demais binários do port.
- Mantém helpers textuais e specs como payload e recusa ELF escondido em
  `nxextract-spec`, `runtime-data` ou `runtime-hook`; nenhum path, suffix ou
  papel genérico passa a autorizar binário arbitrário.
- Corrige o `public-final` para exigir recibo físico GPTK somente quando o
  projeto declara `controls.runtime_mapping = "nxinput-gptk"`. Ações e
  contextos de passthrough nativo continuam obrigatórios no contrato, mas o
  bloco físico de input deve ser `null`; uma prova GPTK inventada é recusada.
- Preserva integralmente a prova GPTK evento → sink, exactly-once e os
  símbolos canônicos quando o opt-in está presente. A composição continua
  nxbootstrap 0.7.7, nxinput 0.8.1, nxgl 0.3.4 e nxgenerator 0.3.11.
- A 0.3.14 entra na autoridade histórica externa somente leitura; não há tag,
  release, merge, migração automática de port ou mudança visual.

# 0.3.14 (2026-08-31, composição nxbootstrap 0.7.7)

- Exige nxbootstrap 0.7.7 e nxgenerator 0.3.11, fechando na composição de
  release a regra de que health e vídeo não promovem um filho que saiu não zero.
- A composição do gerador também impede `promotion` de apagar ou trocar o pin
  `input_controller_profiles` que alimenta `controllers.nxb` no runtime.
- Reseala schemas de proveniência e identidade corrente; 0.3.13 entra na
  autoridade histórica externa read-only.
- Alinha o schema da autoridade histórica ao conjunto já aceito pelo runtime,
  agora de 0.2.40 a 0.3.13, com positivo dirigido para o limite superior.
- Preserva integralmente os gates Godot/GPTK, `controllers.nxb`, SDL e pixels
  da 0.3.13; nenhum port antigo migra automaticamente.
- O opt-in Godot corrente passa ao marker `nxgl-godot-frame-proof/2` e exige
  no ELF e na prova os símbolos definidos de fatal persistente e consumo de
  fechamento. O glue 0.3.3 log-only permanece apenas histórico.

# 0.3.13 (2026-08-31, identidade Godot ligada ao ELF)

- A prova GPTK pode declarar, somente como par completo, os marcadores
  `nxinput-godot-runtime/1` e `nxgl-godot-frame-proof/1`. O normalizador
  preserva a extensão e a validação confere ambos nos bytes do executável
  final; campo parcial, valor diferente ou claim sem marcador falha fechado.
- O schema fechado do candidate lock aceita o mesmo par opcional com
  dependência bidirecional; um lock parcial é inválido antes do normalizador.
- Provas não-Godot preservam exatamente o contrato runtime anterior. A
  extensão não cria renderer, contexto, frame ou aprovação física.
- Gate dirigido cobre positivo, campo parcial e ELF sem o marcador declarado.
- A composição corrente exige nxgenerator 0.3.10: runtime nxinput sem o bundle
  pinado da autoridade 3 não chega à fronteira de release.
- O espelho de release exige o nome canônico `controllers.nxb`; um bundle
  renomeado não pode passar o pacote e ficar inalcançável pela declaração NXC6
  no runtime.

# 0.3.12 (2026-08-31, prova GPTK completa e autoridade 3 obrigatória)

- Substitui o falso gate por string `nxinput-gptk-runtime/1`. O renderer exige
  o marker v2, schema de evidência e símbolos definidos da fronteira
  `nxinput_gptk_live`; anexar texto a outro ELF não passa.
- Exige nxgenerator 0.3.9 e adapter promovido com política
  `nxinput-gptk-live/1`: contexto inicial não provado, passthrough nativo,
  cobertura completa de sinks antes da ativação e ACK obrigatório.
- O `candidate-lock` externo ganha o par `input_proof`/
  `input_proof_receipt_sha256`. Ele prende ELF, geração, mapping e contrato e
  fecha todos os contextos, sinks e bindings default em casos
  evento → ACTION → sink → uma entrega. Contexto desconhecido/sink ausente
  precisam provar passthrough sem supressão; ACK falho precisa ser fatal sem
  replay nativo.
- Sinks apontam para símbolos port/engine reais e comprovam targets pelo
  registry de ações em runtime ou por artefato de contrato empacotado e
  autenticado. Marker/load sem dispatch, ação inexistente e sink stale falham.
- Gate dirigido `tests/test_gptk_runtime_proof.py` cobre o bypass histórico,
  o positivo real, dispatch ausente e símbolo de sink inexistente, sem device,
  ZIP ou suíte global.
- Passa a exigir nxgenerator 0.3.9; a 0.3.11 entra na lista histórica
  reproduzível.
- **Espelho do gate novo do gerador**: um adapter-contract com
  `input.runtime_mapping = "nxinput-gptk"` sem `input_controller_profiles`
  habilitado é recusado no empacotamento — a autoridade 3 (bundle
  NXCONTROLLER_PROFILES/1) tem que viajar DENTRO do ZIP (regressão muOS
  31/08/2026 do Nameless Cat 1.2.3, publicado sem bundle).
- As verificações de fechamento do bundle (presença no pacote, SHA-256
  byte-exato, cabeçalho, licença, ausência de dado pessoal) permanecem as da
  0.3.8+, inalteradas.

# 0.3.11 (2026-08-30, nxbootstrap 0.7.6 + provider SDL do sistema)

- Alinha `VERSION`, `TOOL_VERSION`, schemas de proveniência e contratos na
  versão 0.3.11; a 0.3.10 entra na lista histórica reproduzível em vez de ser
  reapresentada como corrente.
- Exige a composição nxbootstrap 0.7.6 + nxgenerator 0.3.8. O launcher,
  renderer e geração-v2 precisam concordar com esses pins exatos.
- Reconhece a fronteira supervisionada de `sdl_provider=system` pelo guard do
  filho `NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD`, sem exigir o guard legado
  de `BIN_PRELOAD` que essa rota neutraliza antes do launch.
- Permite somente a restauração canônica de `SDL_DYNAMIC_API` por variáveis
  `NXBOOTSTRAP_SYSTEM_SDL_*`; redirect local, SDL1/SDL2 privada e exceção SDL3
  incompleta continuam recusados.
- Preserva sem alteração semântica o gate 0.3.10 de
  `controls.runtime_mapping = nxinput-gptk` e todos os contratos visuais,
  candidato-lock, geração, privacidade e baixa GLIBC já vigentes.

# 0.3.9 (2026-08-30, seed V4 obrigatório na closure de release)

- Toda generation-v2 publicável passa a exigir no core do NXRelease exatamente
  um seed visível `<port-id>/nxruntime-<generation-id>.nxb`. Não basta o
  renderer tê-lo descoberto nem `GENERATION.json` listar algum arquivo: path,
  kind `nxruntime-seed` e modo `0644` são parte da semântica obrigatória.
- O header `NXBUNDLE1` precisa declarar o mesmo `generation-id` autenticado
  pela geração e o mesmo `port-id` do pacote. Seed ausente, renomeado,
  duplicado ou pertencente a outro port/geração falha antes da publicação.
- A closure do seed precisa ser exatamente a closure do store imutável
  `.nxruntime/generations/<generation-id>/`, com os mesmos paths, modos,
  tamanhos e SHA-256, exceto por `commit`: esse recibo é criado localmente por
  último e continua proibido dentro do `.nxb`. Membro ausente, extra, stale ou
  divergente falha fechado.
- A mesma prova roda sobre os bytes de source, do stage e do ZIP reaberto; uma
  metadata ou receipt coordenado não pode esconder a perda ou substituição do
  seed entre fronteiras. O launcher permanece obrigado a ser o render
  byte-exato do nxbootstrap 0.7.4, que materializa a cache quando `.nxruntime`
  não existe.
- A composição permanece nxbootstrap 0.7.4 + nxgenerator 0.3.7. Arquivos
  históricos conservam os próprios bytes exclusivamente no fluxo read-only de
  quarentena e não são reinterpretados, migrados nem tornados publicáveis pela
  política nova.

# 0.3.8 (2026-08-30, provider SDL e lock do candidato comprovado)

- Exige a composição coerente nxbootstrap 0.7.4 e nxgenerator 0.3.7, que
  introduz os campos pinados `sdl_provider`/`video_proof` no schema gerado.
- Proíbe SDL1/SDL2 privada em release público por basename, SONAME/identidade
  ELF, provider `package`, redirecionamento shell, `dlopen` local e qualquer
  RPATH/RUNPATH (este último já era recusado globalmente). `DT_NEEDED` SDL2
  continua válido somente pelo provider PortMaster/sistema.
- Fecha a exceção SDL3 contra um ELF Linux real: exige path/mode/ABI/SONAME,
  símbolos SDL3 core, versão/source/licença/razão, dependência package e SHA
  exatos. Texto renomeado não passa. O scanner cobre também add-ons
  gfx/gpu/sound, símbolos hidden e archives estáticos quando auditáveis.
- Introduz `nxrelease-candidate-lock-v1` externo, somente leitura e fora da
  árvore do port. A leitura usa `O_NOFOLLOW`, `fstat` antes/depois, inode
  estável, parent seguro, owner exato, `nlink=1` e modo congelado. `stage`,
  `build`, `bundle` e `public-final` ligam a autoridade original ao executável
  de source, stage e ZIP reaberto; hardlink e TOCTOU falham fechado.
- O opt-in `video_proof: "required"` exige no ELF os símbolos
  `nxgl_frame_proof_before_present` e `nxgl_frame_proof_publish`, além dos
  literais `NXBOOTSTRAP_VIDEO_FILE`, `org.nextos.nxruntime.video-proof` e
  `VIDEO-PROOF:`. O lock passa a exigir receipt v1 `OK/non-black`, com SHA da
  linha JSON compacta e igualdade de port/generation; ausência, BLACK, hash
  inventado ou geração stale falham fechado.
- Gate dirigido `tests/test_provider_lock.py` é chamado pela suíte canônica
  `tests/test_nxrelease.sh`, portanto não pode existir apenas como teste solto.
- Metadata interna nunca seleciona política. Histórico 0.2.40–0.3.7 só abre
  read-only por autoridade externa que fixa SHA do archive e versões
  tool/bootstrap, com `quarantine: true` e path de quarentena; o resultado é
  explicitamente não publicável e nunca entra em `public-final`. A promoção
  corrente exige geração embutida, pins exatos e
  `physical_support_proven=true`; in-progress falha fechado.
- Remove o build-B obrigatório. `public-final` executa uma única compilação
  limpa e compara-a ao candidato testado. `build`/`bundle` criam um stage/ZIP
  por padrão; `--prove-deterministic` é somente restage/repackage diagnóstico
  dos mesmos inputs, nunca segunda compilação.
- O lock/receipt é documentado honestamente como âncora procedural: prende os
  bytes exatos emitidos pelo produtor e o run, mas não se apresenta sozinho
  como prova criptográfica de observação humana do display.

# 0.3.7 (2026-08-30, composição integrada da V4 aberta)

- Exige nxbootstrap 0.7.3 (composição runtime-data 0.7.2 + rollback
  real-crash 0.7.1) e nxgenerator 0.3.6 (runtime gerenciado 0.3.5 +
  catálogo do site 0.3.3).
- Candidatos autenticados 0.3.6 entram na lista de reprodução pública; o
  promotor executante identifica 0.3.7. Nenhum outro gate muda.

# 0.3.6 (2026-08-30, closure de runtime gerenciado)

- Adota nxbootstrap 0.7.2 e nxgenerator 0.3.5 para o papel aditivo
  `runtime-data` da generation-v2.
- Classifica o membro live e sua cópia imutável como payload não ELF, exigindo
  igualdade de path, modo `0644` e SHA-256 no receipt completo.
- Preserva a exceção estreita de versões estruturadas `.deps.json` da 0.3.5 e
  a reprodução autenticada dos candidatos anteriores.

# 0.3.5 (2026-08-30, versões estruturadas do runtime .NET)

- O scanner de privacidade deixa de interpretar `assemblyVersion` e
  `fileVersion` de quatro partes dentro de um `*.deps.json` como IPv4. Esses
  campos são metadados estruturados do host .NET, por exemplo `4.6.1.0`.
- A exceção é estreita: vale somente para essas duas chaves em `.deps.json`.
  Endereço em qualquer outro campo, endereço posterior no mesmo documento e o
  mesmo literal num JSON comum continuam falhando fechados.
- Candidatos autenticados 0.3.4 permanecem reproduzíveis; candidatos novos
  declaram 0.3.5.

# 0.3.4 (2026-08-30, runtime vazio compatível com PortMaster legado)

- Adota nxgenerator 0.3.4 e o contrato PortMaster v3.
- Aceita `port.json` v4 sem `attr.runtime`, normalizando essa ausência para a
  lista vazia lógica durante a auditoria.
- Recusa `attr.runtime: []`: a forma vazia canônica agora é a omissão, pois a
  lista explícita quebra o instalador oficial `2024.03.10-0841`.
- Preserva a validação estrita de tipo, strings únicas e listas não vazias, além
  da reprodução de candidatos congelados 0.3.3 e anteriores.

# 0.3.3 (2026-08-30, scanner de privacidade por membro do nxbundle-v1)

- O `nxruntime-seed` deixa de ser classificado como um único documento textual
  pelos primeiros 4 KiB do índice ASCII. O NXRelease agora valida a estrutura
  canônica do `NXBUNDLE1`, autentica o SHA-256 de cada membro e reinicia a
  classificação texto/binário em cada fronteira declarada.
- Isso elimina o falso positivo observado no runtime Godot 3, cujo ELF contém
  o endereço multicast público SSDP `239.255.255.250`, sem relaxar o gate:
  endereço IPv4 em membro textual, path privado mesmo em ELF, payload
  truncado, digest divergente, offset não contíguo e membro duplicado continuam
  falhando fechados.
- A reprodução de candidatos 0.3.2 já congelados permanece explicitamente
  aceita; a versão do executor e de novos candidatos passa a ser 0.3.3.

# 0.3.2-C4 (2026-08-29, V4-CONTROLLERS-03/C4: closure do NEXTOSCONTROLLERS v2)

> Mesma versão 0.3.2 (linha ainda não lançada): a missão 115 exige manter a
> versão proprietária e não criar sucessor só para esta categoria.

- `defaults/NEXTOSCONTROLLERS.gptk` pode declarar `NEXTOS_CONTROLLERS/1`
  **ou** `/2`; antes só `/1` era aceito e um port V2 seria recusado.
- **Closure contra o adapter-contract do mesmo pacote**: toda ação que o
  default liga precisa existir em `input.actions` **com pelo menos um sink
  real**. Um controle ligado a uma ação que ninguém consome é um controle
  morto que o dono não distingue de um funcionando.
- **Cada controle aparece uma única vez por seção**; duplicata falha fechada,
  para que a ordem do arquivo nunca decida.
- Em `/2`, toda seção precisa listar **os 18 controles** — o dono vê o pad
  inteiro. `null` e `native` só são aceitos em `/2`; num arquivo que declara
  `/1` eles falham fechados.
- **Compatibilidade preservada de propósito**: um contrato *scaffold* (sem
  ações declaradas, `release_ready` falso ou geração legada schema 1/2 com o
  default genérico) e um contrato de forma legada não têm allowlist contra a
  qual fechar — a verificação de ação não se aplica a eles, mas as regras
  ESTRUTURAIS continuam valendo. Um default `/2` nessas posições é
  contradição e falha fechado: v2 é opt-in de port schema 3, que sempre
  declara suas ações na forma documentada.
- Gate dirigido novo `tests/test_c4_gptk_closure.py` (5 positivos, 10
  negativos), com contagens derivadas do que executou.

# 0.3.2 (2026-08-29, V4-CONTROLLERS-03/C3: closure do bundle NXCONTROLLER_PROFILES/1)

- Quando o `adapter-contract` declara `input_controller_profiles` habilitado,
  o pacote precisa CARREGAR o bundle: presença do arquivo no `port_dir`,
  header `NXCONTROLLER_PROFILES/1`, header de licença upstream, nada de
  endereço/path pessoal/hostname/`latest`, e nome de arquivo simples (sem
  separador de path). Declaração desabilitada não pode fixar bundle nenhum.
- **Missão 114A:** o pin passou a ser conferido contra os BYTES em disco
  (SHA-256 recalculado do arquivo empacotado), não apenas contra o que o
  registro do pacote afirma sobre eles. Um registro que mente sobre o
  conteúdo agora falha fechado.
- **Missão 114A:** o positivo do gate consome o bundle REAL selado da C3
  (`tests/fixtures/controllers.nxb`, SHA-256
  `a578a7d82d47e681ad7a1cbe48bb49327dad6dbe1c808e46ca3485dbab0dae43`, 526
  GUIDs, MIT/zlib upstream via PortMaster-GUI), não mais um `GOOD_BUNDLE`
  sintético de três linhas; **todos** os negativos são mutações desses mesmos
  bytes reais. O caminho pode ser sobrescrito por `NX_CONTROLLERS_NXB`, e o
  gate recusa qualquer arquivo cujo SHA-256 não seja o selado.
- **Missão 114A:** o resumo do gate deixou de imprimir contagens cravadas à
  mão e passou a DERIVÁ-LAS do que realmente executou.

# 0.3.1 (2026-08-29, V4-GRAPHICS-04: fronteira post-first-present fail-closed)

- O parser da linha `GRAPHICS-EVIDENCE` passa a extrair os campos de extensão
  `phase`, `first_present` e `pre_drawable` (o nxgl 0.3.1 os anexa à linha
  clássica; chaves extras já eram toleradas, então nada muda para receipts
  antigos).
- Quando o `adapter-contract` declara
  `graphics.evidence_boundary=post-first-present` (nxgenerator 0.3.1), cada
  `graphics_proof` só promove se o receipt vinculado atestar
  `phase=post-first-present`, `first_present=1`, o diagnóstico `pre_drawable`
  e uma identidade de commit do framework não vazia e **consistente entre
  todos os proofs**; um receipt reutilizado entre devices (mesmo `run_id`)
  falha porque o receipt final é one-shot. Receipt pré-present, `drawable=1x1`,
  ausência de primeiro present, duplicata e divergência de identidade nunca
  promovem. `AWAITING_FIRST_PRESENT` não existe como verdict promovível.
- `evidence_boundary` com qualquer valor diferente de `post-first-present`
  falha fechado; ausência do campo preserva o gate anterior byte a byte.
- Pin do nxgenerator elevado para 0.3.1; candidatos 0.3.0 já testados
  continuam reproduzíveis (`PUBLIC_FINAL_REPRODUCIBLE_METADATA_VERSIONS`
  mantém 0.3.0).
- Testes novos no `test_nxrelease.sh`: positivo do boundary, legado intacto,
  e negativos de valor desconhecido, receipt pré-present, `first_present`
  ausente, `pre_drawable` ausente, commit ausente/divergente, `1x1` e receipt
  reutilizado.

# 0.3.0 (2026-08-29, V4-REPRO-01 obrigatório e opt-ins declarativos V4)
- **O gate do recibo DISPLAY passou a usar o emissor de verdade.** O parser
  recusa um `display_proofs` escrito à mão exatamente porque só o runtime pode
  dizer o que o adapter instalou — e todos os casos deste gate alimentavam o
  parser com strings digitadas aqui. Agora o gate compila `nxgl_display.c`,
  roda o emissor real e faz o parser ler os bytes que ele imprimiu, exigindo
  não só que sejam aceitos mas que os números batam (640x480 preservado em
  1280x720 = `content=160,0+960x720`).
  Isso não era teatro: **trocar `internal` por `drawable` no formato do emissor
  passa o gate do nxgl inteiro** (`nxgl_v4=PASS`), porque lá o recibo é
  conferido por prefixo e substring. Só a ligação emissor→parser pega. Sem ela,
  uma deriva de formato faria o front de release recusar todo recibo real — ou
  aceitar um recibo que já não significa o que diz.


- **A prova de reprodutibilidade deixa de ser uma flag.** `build` e `bundle`
  sempre reencenam o stage e reempacotam do zero em área temporária privada. A
  antiga `--prove-deterministic` continua aceita para não quebrar script
  antigo, mas não muda mais nada: uma flag que ninguém passa não protege
  ninguém, e os builds que mais precisavam da prova eram exatamente os que
  nunca a passavam.
- A comparação deixa de ser só o SHA-256 do ZIP: **todo ELF autenticado** é
  comparado individualmente. Uma divergência agora **nomeia o ELF** que parou
  de reproduzir, que é a diferença entre um vazamento de toolchain corrigível e
  um mistério. Inventário diferente, bytes diferentes ou ZIP diferente falham
  fechado.
- `build` e `bundle` emitem `BUILD-PROVENANCE.json` sanitizado, no-replace,
  `0644`, com identidade do pacote, toolchain (Python, máquina,
  `SOURCE_DATE_EPOCH`, umask, TZ, locale) e o resultado da dupla construção com
  o hash de cada ELF. O `bundle` publica um diretório novo e usa o nome
  canônico; o `build` grava um ZIP solto num diretório que pode já ter outros
  artefatos e por isso pareia o arquivo com o ZIP, exatamente como o
  `<archive>.sha256` já fazia.
- O documento da promoção `public-final` continua com o schema congelado
  `nxrelease-build-provenance-v1`. O fluxo canônico ganha o schema próprio
  `nxrelease-canonical-build-provenance-v1`, para que um validador nunca
  precise adivinhar qual documento está lendo.
- Valida os opt-ins declarativos V4 escritos pelo nxgenerator 0.3.0 no
  `adapter-contract.json`: forma de `display`, `egl_binding` e
  `input_sdl3_portmaster`, mais a única consequência de pacote que o nxrelease
  realmente enxerga — **um `egl_binding` ligado proíbe `DT_NEEDED libEGL` em
  qualquer ELF do pacote**. Ligar EGL em tempo de execução e linká-la
  estaticamente são afirmações contraditórias, e embarcar as duas
  restauraria em silêncio a dependência global que a frente remove.
- Corrige uma expectativa obsoleta em `tests/test_nxrelease.sh`, que ainda
  exigia a mensagem `nxbootstrap 0.6.36` depois do bump para 0.6.37 e por isso
  reprovava a própria suíte na V3 congelada. A V3 não é tocada; a correção vive
  aqui.
- **V4-DISPLAY-01, exigência que faltava:** um port que declara
  `display.remap_input` está afirmando algo sobre para onde vai o dedo do
  jogador. Agora isso exige `display_proofs` com o **recibo que o runtime
  realmente imprimiu** (`DISPLAY: policy=… internal=… drawable=… content=…`),
  um por device, sem duplicata, com a política e o tamanho interno batendo com
  o que o port declara e com content rect não vazio. Um veredito escrito à mão
  não passa. `remap_input` falso não exige recibo nenhum.
- Novos gates `tests/test_v4_repro.py` e `tests/test_v4_optins.py`.

# 0.2.43 (2026-08-28, composição nxbootstrap 0.6.37)

- Avança as identidades canônicas para nxbootstrap 0.6.37 e nxgenerator
  0.2.20, preservando o NXExtract 1.2.21 e a NXSplash 0.1.2.
- Novos candidatos precisam carregar os source pins e a generation-v2
  derivados dessa composição; artefatos 0.2.42 e anteriores não são
  reinterpretados nem regenerados.
- `public-final` mantém a reprodução fechada de candidatos autenticados
  0.2.40, 0.2.41 e 0.2.42, enquanto o gate e `BUILD-PROVENANCE.json`
  identificam o executor corrente como 0.2.43.
- Não relaxa o marcador transacional `commit`: geração sem esse controle
  continua truncada e falha fechada. A correção 0.6.37 cobre somente modos
  POSIX alterados depois da extração.

# 0.2.42 (2026-08-28, reprodução exata do candidato físico)

- `public-final` agora reproduz um ZIP físico já congelado por versões
  anteriores explicitamente compatíveis sem reescrever a identidade do
  NXRelease dentro de `NXRELEASE-METADATA.json` e `SBOM.cdx.json`.
- A versão do artefato vem exclusivamente da metadata autenticada pelo
  `verify_archive`; não existe override externo. Somente 0.2.40, 0.2.41 e a
  versão corrente 0.2.42 são aceitas, e tested/build-A/build-B continuam
  obrigatoriamente iguais byte a byte.
- O gate e `BUILD-PROVENANCE.json` sempre declaram 0.2.42. Builds ordinárias
  também continuam emitindo 0.2.42; a compatibilidade antiga só existe dentro
  da reprodução fechada de `public-final`.
- Acrescenta regressão dirigida para a seleção 0.2.40, projeção idêntica em
  metadata/SBOM e recusa de versão não suportada. Não muda launcher, runtime,
  NXExtract, NXSplash, interface visual ou qualquer port.

# 0.2.41 (2026-08-28, atualização preserva rollback autenticado)

- Corrige o falso negativo do ciclo HarbourMaster em updates schema 3: a
  geração anterior completa sob `.nxruntime/generations/<id>/` é rollback A/B
  quando v2 e control-only inerte quando v1, nunca lixo genérico a apagar.
- A exceção é estrita: `GENERATION.json`, id, manifesto, commit, inventário,
  ordem, modos e bytes do ZIP anterior precisam autenticar exatamente a closure
  retida; o mesmo id só reaparece com path/modo/SHA-256 idênticos.
- Preserva a migração real: primeira adoção do store por um ZIP antigo sem
  geração, v1 de 32 hex com `execution_roles` mixed-ABI e a ordem histórica por
  componentes de path até nxgenerator 0.2.18; 0.2.19+ exige ordem POSIX textual.
- Arquivo comum aposentado, root de geração extra, closure parcial, receipt ou
  store órfão, commit ou hash divergente continuam falhando antes do device;
  estado split nunca é confundido com ZIP simples nem com primeira adoção.
- Acrescenta regressões de v1 legado com id de 32 hex, v2 completa, mesmo id,
  adoção inicial, `execution_roles`, closure parcial, receipt/store órfão,
  ordem, modo, token único do launcher e arquivo apenas parecido com store. Não
  muda runtime, launcher, NXExtract, NXSplash ou UI.

# 0.2.40 (2026-08-28, modo autenticado de biblioteca privada)

- Exige exatamente nxgenerator 0.2.19, que ordena o inventário do recibo por
  path POSIX e elimina a divergência entre a ordenação por componentes de
  `Path` e a ordenação lexical validada pelo renderer.
- O renderer preserva o modo já autenticado de `private-library` durante a
  classificação convencional de `lib/*.so*`; um ELF live/store `0644` não é
  mais reclassificado silenciosamente pelo default histórico `0755`.
- A igualdade live/store continua fail-closed para path lógico, modo e SHA-256,
  e a cópia imutável continua herdando metadados ELF completos do membro live.
- O candidato real dirigido agora cobre executável, biblioteca privada `0644`,
  NXSplash e NXExtract UI duplicados no generation store, além dos negativos
  independentes de drift de modo `0755→0644` e `0644→0755`.
- Não altera schema de release, NXExtract, NXSplash, launcher, interfaces
  gráficas ou ports e não produz ZIP nem alegação física.

# 0.2.39 (2026-08-28, composição autoral antes do recibo)

- Exige nxgenerator 0.2.18 e valida o `package_payload` schema 3 como parte da
  árvore package-shaped: path, kind, modo e SHA-256 precisam concordar com os
  bytes finais já ligados por `GENERATION.json`.
- O renderer inclui payloads declarados ainda não conhecidos por convenção e
  confere os já conhecidos; adulteração, omissão, duplicata ou overlay posterior
  falha antes de `stage`.
- O recibo do gerador precisa ligar o SHA-256 do projeto e exatamente a mesma
  closure final de artefatos; composição coordenada depois da geração também
  falha fechada. SHA-256 autoral em maiúsculas é recusado como não canônico.
- ELFs duplicados no generation store usam `nxruntime-generation-linux`,
  herdam arquitetura, perfil, `DT_NEEDED`, `SONAME` e proveniência do membro
  live e atravessam a mesma auditoria ELF/GLIBC; path lógico, modo e SHA-256
  live/store precisam ser idênticos. Controles não ELF preservam a classe
  `nxruntime-generation`.
- `nx-refresh-pins.py` passa a recalcular também os hashes do payload autoral,
  preservando a mesma fronteira sem symlink/hardlink usada pela closure de
  runtime.
- O gate de candidato real cobre documentação autoral, versão, proveniência,
  ferramenta aninhada e tuning GPTK declarativo. Nenhum ZIP nem alegação física
  é produzido por esses testes de host.
- Não muda NXExtract, NXSplash, launcher ou qualquer interface gráfica e não
  migra ports automaticamente.

# 0.2.38 (2026-08-28, composição NXExtract da geração v2)

- Alinha a identidade obrigatória ao nxgenerator 0.2.17, que reutiliza e
  autentica recipe, engine, runner, runtime-env e UI já materializados pelo
  nxbootstrap 0.6.36 na closure de geração v2.
- Um candidato schema 3 com NXExtract real passa a alcançar normalmente o
  renderer e os gates `validate`/`stage`/`verify-stage`; bytes ou modos
  NXExtract não canônicos continuam falhando antes de qualquer publicação.
- Registra na matriz executável da bateria os três gates dirigidos de geração
  v2, candidato package-shaped e refresh schema 3; arquivos de teste novos não
  podem mais ficar fora de `run-safe-gates.sh`.
- Não altera schema de release, interface gráfica, launcher, NXExtract,
  NXSplash, formato de receipt nem migra qualquer port automaticamente.

# 0.2.37 (2026-08-28, geração v2 fechada e promoção run-bound)

- Fecha o falso `public-final` que aceitava `lifecycle.completed=true` mesmo
  quando o launcher registrava `NXU0005` e deixava a geração eternamente
  `pending`.
- O receipt físico passa a exigir `lifecycle.health_evidence` exatamente igual
  ao `UPDATE NXU0006` emitido pelo nxbootstrap após validar o health receipt
  privado 0600 desta execução e desta geração.
- Recusa ausência do campo, `NXU0005`, `NXU0007`, run divergente e generation
  divergente. BB1/BB2 do incidente ficam inválidos até integrarem um produtor
  runtime; ports históricos aprovados não são migrados.
- Corrige a ordem do build reproduzível: a leitura inicial autoriza somente
  `source_root`, epoch e a lista exata de saídas `project-linux`; o ELF externo
  é construído primeiro e só então o manifesto completo é validado contra seus
  bytes. Uma worktree limpa pode, portanto, omitir ELFs ignorados sem reduzir
  nenhuma checagem de hash, ABI, GLIBC, dependências ou inventário.
- O vínculo externo é fechado um-para-um: recusa saída ausente/extra, symlink,
  não-ELF, hash divergente, override de payload não-project e qualquer input
  não-project ausente. O build não recebe autoridade para substituir scripts,
  dados do dono ou componentes do framework.
- A limpeza das duas worktrees passa a incluir também arquivos ignorados pelo
  Git. Um build que deixe ELF, cache ou `.build` ignorado dentro da fonte falha
  antes/depois da execução; somente o diretório externo autorizado pode receber
  os ELFs reproduzíveis.
- Fecha a brecha de hooks cujo entrypoint parecia genérico, mas chamava outro
  script/import/spec JSON preso a um único payload interno. A auditoria agora
  percorre recursivamente a closure textual empacotada e aplica a defesa
  APK-COMPAT também aos módulos e dados alcançáveis.
- SHA interno de entrada nessa closure só é aceito quando declarado como
  `patch_profiles.match_internal_payload`: o NXExtract autentica e seleciona o
  perfil durante o plano, antes da extração cara, e a build desconhecida segue
  um fallback genérico. Fallback livre ou com semântica de rejeição falha.
- Integra por opt-in o `nxbootstrap 0.6.36` e o `nxgenerator 0.2.16`: schema 3
  sela launcher, executável, bibliotecas, hooks e a closure NXExtract completa
  numa geração v2 imutável. Receita, engine, runner, ambiente, UI, helpers e
  specs transitivos são publicados e curados como uma única unidade; estado
  híbrido ou interrompido falha antes de executar extrator/jogo.
- O renderer autentica os sete controles e todos os bytes live/store da geração
  v2, sem teto ou nomes específicos de jogo. O `verify` comum repete essa prova
  sobre o ZIP real, portanto nenhum build helper pode simular o selamento por
  comentário, marcador ou manifesto parcial.
- O build schema 3 compila primeiro, recalcula os hashes da closure e produz um
  candidato package-shaped novo via nxgenerator; não depende de
  `nxrelease.json` antigo na fonte. `runtime_root`, caminhos intermediários e
  membros finais recusam symlink/hardlink antes de qualquer hash ser atualizado.
- Completa o draft `public-final-receipt-v1`, ainda não publicado por tag. A
  geração v2 é opt-in; ports históricos não são regenerados nem migrados.

# 0.2.36 (2026-08-28, reabertura autenticada do stage)

- Corrige `verify_stage`: records reconstruídos do inventário autenticado
  agora preservam o `sha256` que já foi conferido contra o arquivo do stage.
- Elimina o falso negativo que recusava
  `defaults/NEXTOSCONTROLLERS.gptk` e `defaults/NEXTOSSETTINGS.txt` válidos como
  "não pinados" somente durante `stage`/`build`/`bundle`.
- Acrescenta regressão de ponta a ponta com geração, GPTK e settings válidos
  atravessando build, reabertura, segunda build determinística e bundle; pin
  ausente ou divergente continua falhando fechado na fronteira de entrada.
- Não altera o schema 2, os formatos de metadata/SBOM/receipt nem migra ports
  automaticamente.

# 0.2.35 (2026-08-27, integração runtime canônica)

- Corrige o gate `public-final` para exigir somente símbolos reais do
  nxinput 0.5.1: carregamento owner/default seguro, receipt do loader, parser,
  registro do sink, source guard, máscara primária e despacho com origem.
- Recusa receipts que tentem satisfazer GPTK com os nomes inexistentes usados
  pela fixture 0.2.34 e mantém obrigatória a prova de uma única entrega.
- Exige `nxaudio_receipt_format()` e `nxaudio_liveness_tick()` em todo adapter
  que declara áudio; quando o receipt afirma recuperação, exige também o ciclo
  limitado real e seu receipt do nxaudio 0.3.1.
- Avança a identidade canônica para nxbootstrap 0.6.35 sem alterar o schema 2
  nem migrar automaticamente ports já aprovados.

# 0.2.34 (2026-08-27, gate public-final fail-closed)

- Adiciona o subcomando explícito `public-final`. `validate`, `stage`, `build`,
  `bundle` e `verify` continuam sendo gates estruturais/de desenvolvimento e
  não promovem scaffold nem alegam suporte físico. O template passa a nomear
  esse resultado `DEV/PACKAGE PASS`.
- O gate final exige `nxproject` schema 3 promovido, adapter
  `implemented_release`, `release_ready=true`, lifecycle com sequência e
  evidência, e igualdade entre nxproject, adapter, claims de geração,
  `nxport.json` e o manifest da geração runtime. `physical_support_proven`
  pode continuar falso no fonte: a promoção física nasce somente dos receipts
  externos posteriores ao ZIP imutável.
- Cada receipt sanitizado identifica o mesmo SHA/tamanho do ZIP, SHA/build-id
  do ELF, generation id e commit. Integrações declaradas exigem símbolos e
  prova runtime: contrato/shader/frame para gráficos, GPTK→dispatcher→sink com
  `delivery_count=1`, callback de áudio vivo e SELECT+START independente. Um
  frame proof conclusivo `BLACK` reprova a release.
- A reprodutibilidade final deixa de ser o antigo duplo empacotamento da mesma
  árvore: o próprio comando executa o mesmo build script versionado e constrói
  em duas worktrees Git distintas, limpas e no mesmo commit, compara os três
  ZIPs byte a byte (testado/A/B), compara inventários e todos os ELFs e só então
  cria `BUILD-PROVENANCE.json` sem sobrescrever destino. O contrato
  `NX_PUBLIC_FINAL_OUTPUT_DIR` mantém os ELFs reconstruídos fora da árvore; só
  aceita exatamente os paths `project-linux` do manifesto.
- Novos artefatos usam `nx-render-manifest.py --public-final` e carregam
  `<port>/GENERATION.json`. Existe uma migração explícita para recibo de geração
  externo de ZIP root-less, mas ela não dispensa receipts exatos nem duas
  builds idênticas. ZIPs históricos já aprovados permanecem congelados e não
  recebem retroativamente `PUBLIC-FINAL-PASS`; não há rebuild/reteste ou
  migração automática de port.

# 0.2.33 (2026-08-27, NXExtract 1.2.21 e nxbootstrap 0.6.34)

- Promove as identidades canônicas para nxbootstrap 0.6.34, NXExtract 1.2.21 e
  nxgenerator 0.2.15; 1.2.20 continua registrado para ZIPs já publicados.
- Fixa os hashes exatos do novo engine/runner e mantém a UI NXExtract 1.2.16,
  a NXSplash 0.1.2 e todos os contratos visuais byte a byte inalterados.
- O bump acompanha a mudança funcional e elimina o estado em que constantes e
  `VERSION` poderiam apontar versões diferentes.

# 0.2.32 (2026-08-27, hooks NXExtract do port no pacote)

- `nx-render-manifest.py` passa a materializar no manifesto cada arquivo
  regular próprio do port colocado diretamente em `nxextract/`, além dos quatro
  componentes canônicos. Isso fecha a falha de campo em que a receita chamava
  `{game_dir}/nxextract/<hook>`, mas o ZIP não continha o script e morria no
  device com `can't open file`.
- Hooks `.py`/`.sh` recebem modo `0755`; dados auxiliares recebem `0644`.
  Symlinks, bytecode `.pyc`, `__pycache__` e os quatro arquivos canônicos não
  são admitidos pelo caminho adicional.
- Regressão host exercita inclusão, modo e exclusões no renderer. O schema de
  release, a UI NXExtract, a NXSplash e os demais contratos visuais não mudam.

# 0.2.31b (2026-08-27, alinhamento nxbootstrap 0.6.32)

- O launcher canônico exigido passa de 0.6.31 para 0.6.32 (rollback de geração
  imutável + health receipt). A árvore candidata e o gate voltam a concordar.
- `GENERATION.json.generation_id` é validado como o SHA-256 completo de 64 hex
  emitido pelo gerador; 32 hex é aceito apenas como leitura de migração de
  gerações antigas, nunca como identidade nova.

# 0.2.31a (2026-08-27, V3-GRAPHICS-02 item 5)

- Correção de integração pré-release da NXExtract 1.2.20: o gate APK-COMPAT
  passa a recusar também `source_validate`/`output_validate`, filename literal e membros de
  assinatura/certificado. A defesa estática agora abre e inspeciona a fonte
  local de hook efetivamente referenciada no inventário, em vez de olhar apenas
  `argv`/`env` (NXA0055).
- O gate gráfico de release deixou de aceitar `verdict=OK`/`shader_probe=pass`
  escritos à mão. Cada prova em `graphics_proofs` agora precisa LIGAR a um recibo
  físico estruturado `GRAPHICS-EVIDENCE` (`parse_graphics_evidence`): a linha tem
  de carregar `generation=<generation_id desta geração>` e um `build_id` real
  (DSO medido), e o resumo da prova (verdict/reason/shader_probe/obtained/
  drawable) não pode divergir da linha de evidência. Além disso: rejeita provas
  duplicadas por device; valida a política de versão contra a versão OBTIDA
  (exact/minimum/range, com version_max coerente na declaração); e exige
  cobertura de `graphics.required_devices` quando declarado. positive=3
  negatives=20 (antes 13).

# 0.2.31 (2026-08-26, onda V3)

- Validacao de geracao V3 fail-closed (auditoria V3, ponto 4): uma geracao com
  generation_id passava mesmo sem `adapter/adapter-contract.json` ou com o JSON
  malformado (o bloco era pulado em silencio), e nao exigia
  `defaults/NEXTOSSETTINGS.txt` nem conferia o gptk alem da magic. Agora o
  contrato e' OBRIGATORIO e bem-formado (ausente/malformado = falha), o
  `language_access` e' nao-nulo e coerente com `supported`/`fallback`/`sinks`
  (listas), `input.actions` e' lista e um contrato `release_ready` declara ao
  menos uma acao; o `defaults/NEXTOSSETTINGS.txt` e' exigido e validado pelo
  MESMO contrato do runtime (allowlist language/quality, valor
  [A-Za-z0-9._-]{1,32}, quality no allowlist) e o gptk exige secao e ao menos
  um binding. Cobertura nova em `tests/test_nxrelease.sh` (positivo + 6
  negativos).
- Gate de release do contrato gráfico (V3-GRAPHICS-02, step 3): quando o
  adapter-contract declara um bloco `graphics` (api/profile/version/policy/
  shader_dialect), ele é validado bem-formado; e um port `release_ready` com
  `physical_support_proven` DEVE trazer `graphics_proofs` (um por device) cujo
  verdict é OK -- falha fechada se: contexto desktop-GL para contrato GLES
  (`obtained_api != gles`), drawable 1x1, shader-probe do dialeto declarado
  falhou, verdict FAIL, ou ausência de prova física. O contrato é a declaração
  do PRÓPRIO port; workaround de jogo nunca vira default global. Cobertura em
  `tests/test_nxrelease.sh` (positivo + 6 negativos de graphics).
- V3-UPDATE-01: o export canônico do launcher esperado passa a ser
  `NXCOMPAT_GAME_DIR="$NXBOOTSTRAP_LOGICAL_GAMEDIR"` (identidade lógica do guest).
- Corrige a inconsistencia VERSION 0.2.30 vs TOOL_VERSION 0.2.29: a ferramenta
  agora se identifica 0.2.31 em metadata, SBOM e --version.
- APK-COMPAT-01: a politica de container roda no modulo canonico unico
  framework/contracts/apkcompat (identidade de container -- sha256/crc32 em
  QUALQUER quantidade e size exato -- nunca decide compatibilidade;
  reference_build/compatibility/patch_profiles validados; NXA####).
  A forma legada com whitelist de dois SHAs vira regressao NEGATIVA
  (fixture angrybirds-legacy-identity).
- Hooks sao codigo nao confiavel para release: defesa estatica sobre argv/env
  (64-hex, versao pontuada literal, tabela de tamanhos, offset absoluto) e
  contrato org.nextos.apk-compat.hook-contract/1 obrigatorio em receita V3.
- GAMEDATA-DIR-01: <port>/gamedata/README.txt obrigatorio em pacote NXExtract
  nas tres fronteiras (manifesto fonte, stage, ZIP reaberto), com kind payload,
  modo 0644, hash pinado e conteudo nao vazio; extracao limpa do ZIP final
  prova o diretorio fisico; coerencia INSTALLATION.md + extractor.json
  search_dirs (gamedata primeiro). nx-render-manifest torna o marcador
  obrigatorio com NXExtract ativo e nunca o promete sem NXExtract.
- V3-CONTROLLERS-01: geracao com generation_id exige
  defaults/NEXTOSCONTROLLERS.gptk (payload 0644 pinado, magic
  NEXTOS_CONTROLLERS/1); a copia editavel do dono fora de defaults/ e' estado
  mutavel e nunca entra no ZIP (V3-STATE-01).
- GENERATION.json aceita generation_id (32 hex) do nxgenerator 0.2.13.
- Suite: negativos novos de gamedata (ausente, kind, modo, hash stale, nivel
  extra), negativo de identidade legada e pins atualizados.

# 0.2.30 (2026-08-23, onda v2)

- Identidades canonicas: nxbootstrap 0.6.30 e motor NXExtract 1.2.18 (registry
  atualizado; 1.2.14-1.2.17 continuam supported para ports publicados).
- Nenhum gate muda.

# nxrelease changelog

## 0.2.29 — 2026-08-20

- Acompanha o nxbootstrap 0.6.29 (script de linker ignorado na closure por
  papel): o `launcher_contract.version` canônico exigido passa a ser 0.6.29.

## 0.2.28 — 2026-08-20

- Aceita o contrato aditivo `options` do nxport e exige que ele seja canônico
  (mesma ordem, mesmos campos que o gerador deriva). Passa a exigir
  nxbootstrap 0.6.28.

## 0.2.27 — 2026-08-20

- **Gate do ordinal pad fix.** Uma árvore de fontes que define
  `pad_ordinal_fix_apply` sem o gate de barramento externo passa a ser
  recusada em `validate`/`stage`/`build`/`bundle`. A assinatura herdada
  (`BTN_GAMEPAD` + `BTN_C`/`BTN_Z`) casa com o pad INTERNO do H700, que
  publica esses mesmos códigos já com layout semântico correto: aplicar a
  correção ali troca os botões de um controle que estava certo. Passam o
  header canônico `framework/nxinput/include/nxinput_pad_ordinal_fix.h` e
  cópias locais que ainda decidam por `bustype` aceitando somente `BUS_USB`
  e `BUS_BLUETOOTH`. Um gate alargado para `BUS_HOST` é recusado. Chamada
  isolada não é definição, e sobras em `build/`, `package/` e `gamedata/`
  não são auditadas. Nenhum port é migrado automaticamente: cada um entra
  no seu próximo rebuild.

## 0.2.26 — 2026-08-20

- Adota por opt-in nxbootstrap 0.6.27 e NXExtract 1.2.13, preservando a UI
  gráfica NXExtract 1.2.9 e a NXSplash 0.1.2 byte a byte.
- Audita `execution_roles` por ELF real: ABI, classe, machine, `PT_INTERP` e
  closure são conferidos separadamente para extrator, splash, jogo e helpers.
  Assim, um host AArch64 pode instalar e iniciar um jogo ARMHF no Spruce sem
  contaminar a closure 32-bit com bibliotecas AArch64.
- Aceita o recibo opcional `GENERATION.json` do nxgenerator 0.2.11 e exige que
  seus papéis coincidam exatamente com `nxport.json`. Ports sem
  `execution_roles` mantêm o contrato histórico single-ABI.

## 0.2.18 — 2026-08-18

- Acompanha o nxbootstrap 0.6.18 (preflight do interpretador PT_INTERP): o
  `launcher_contract.version` canônico exigido passa a ser 0.6.18 e `TOOL_VERSION`
  vai a 0.2.18. Nada mais muda no gate.


## 0.2.17 — 2026-08-18

- **Gate de baseline de runtime da CFW.** Uma dependência NEEDed e declarada
  `provider=portmaster` ou `provider=firmware`, quando **não empacotada**, agora
  precisa estar num allowlist curado do que TODA CFW suportada garante. O
  `libzip.so.5` existe no muOS/Knulli (`/usr/lib`) mas **não** no ArkOS puro, e o
  Magic Rampage o declarava como `portmaster` — passava no gate e morria com
  `libzip.so.5: cannot open shared object` (status 127) no ArkOS. Agora o
  release falha cedo, com a instrução de empacotar (`provider=package`) ou
  linkar estático. Baseline portmaster: família SDL2. Baseline firmware:
  loader/GL (`ld-linux-*`, `libEGL*`, `libGLESv2*`, `libGLESv1_CM*`),
  `libgcc_s`/`libstdc++`, `libz.so.1`, `libfreetype.so.6`, `libopenal.so.1`.
  Sonames empacotados no próprio port continuam livres. Verificado contra os
  manifestos de todos os ports publicados: só o Magic Rampage é reprovado.

## 0.2.16 — 2026-08-17

- Follows the canonical generator to nxbootstrap 0.6.17 (CFW identity from the
  plymouth title / os-release, and the NXExtract-phase python3 health probe);
  a release pinned to 0.6.16 fails closed. Nothing else changes.

## 0.2.15 — 2026-08-16

- Follows the canonical generator to nxbootstrap 0.6.16, so a release pinned to
  the previous launcher fails closed instead of quietly accepting a launcher the
  framework no longer generates.
- Added `nx-refresh-pins.py`. Closing a package used to mean regenerating the
  launcher and then chasing a chain of hand-derived hash edits -- the launcher
  contract version, the release tool version and its own SHA, and one source
  hash per file that moved -- until the gate stopped complaining. Every one of
  those is derivable from the file it pins, and a human deriving them is how a
  stale pin reaches a release. `--check` reports staleness and exits non-zero,
  which is what a gate wants. Hashes are substituted in the raw manifest text
  rather than re-serialized, so the diff stays reviewable.
- Added `nx-ship-port.sh`: refresh pins, build and verify the package, install
  it on each requested device and prove it drew a real frame, in one command. A
  device that cannot prove the image fails the whole run. Verified on Swordigo
  1.0.15 across three firmwares -- RK3326/dArkOS, NextOS Mali-450 and Amlogic
  X5M -- each reaching an ES-CM 1.1 context with a measured frame proof.

## 0.2.14 — 2026-08-16

- Adota por opt-in nxbootstrap 0.6.15 e o engine NXExtract 1.2.10, exigindo
  resultado terminal estruturado e fronteiras de fase preservadas no launcher.
- Mantém a interface gráfica NXExtract 1.2.9 byte-idêntica e registra sua
  versão separadamente da versão do engine nos metadados internos do ZIP.
- Remove a cópia local obsoleta do bloco NXExtract: a auditoria agora compara
  diretamente a renderização do gerador canônico e depois o launcher completo.
- Reabre o ZIP e revalida versão do engine, versão visual, hashes, ABI, GLIBC,
  PortMaster real, instalação bilíngue e ausência do comando externo `stat`.
- Atualiza o lock declarativo global para contrato 1.0.33/nxrelease 0.2.14;
  nenhum port histórico é migrado automaticamente.

## 0.2.13 — 2026-08-16

- Fixa o contrato PortMaster-GUI/HarbourMaster real pelo snapshot imutável
  `8f9ddc4`, sem rede e sem executar launcher ou ELF durante o ciclo de pacote.
- Exige `port.json` schema v4 e valida `items`, `items_opt`, `attr.runtime`,
  `attr.arch`, `attr.min_glibc` e `attr.title` contra os bytes reais do ZIP.
- Impõe `<port-id>/INSTALLATION.md` pinado, modo `0644`, com seções em inglês e
  português, repetindo o gate na fonte, no stage e no ZIP reaberto.
- Rejeita BOM, chaves duplicadas, `NaN`, truncamento e vírgula final no
  manifesto e nos metadados internos do arquivo final.
- Adiciona `verify --previous-archive` para provar install, discovery, restart,
  update, ausência de órfãos, uninstall restrito e reinstall determinístico.
- Preserva os contratos nxbootstrap 0.6.14, NXExtract 1.2.9 e NXSplash 0.1.2;
  nenhum port histórico é regenerado automaticamente.
- Atualiza o lock declarativo global para contrato 1.0.29/nxrelease 0.2.13, de
  modo que a suíte isolada recuse qualquer divergência entre `VERSION` e pins.

## 0.2.12 — 2026-08-15

- Adota por opt-in nxbootstrap 0.6.14 e NXExtract 1.2.9, mantendo NXSplash
  0.1.2 e os bytes do launcher/core aprovados.
- Audita todo shell allowlisted, inclusive helper sem extensão identificado por
  shebang, e recusa o comando externo `stat` em prepare, runner, runtime e
  helpers.
- Chama o auditor independente `audit-portmaster-zip.py` sobre os bytes do ZIP
  real tanto no build quanto no verify, recusando também `nxbootstrap.sh`
  aposentado.
- Aceita container sem hash externo apenas com package e âncora interna forte
  por SHA-256 ou estrutura; preserva Angry Birds, Retro Highway e o container
  transformado sem bounds/magic do ScourgeBringer.
- Mantém evidência física separada: captura gráfica válida apenas no Mali-450;
  o alvo ArkOS-class/KMSDRM tem receipt de renderer, não prova visual. Matrizes
  sintéticas permanecem `hardware_ran=false`.

## 0.2.11 — 2026-08-15

- Adota, somente por opt-in, nxbootstrap 0.6.13, NXExtract 1.2.8 e nxsplash
  0.1.2; versões anteriores continuam fixadas ao NXRelease que as aprovou.
- Substitui o hash AArch64 global da UI pelo manifesto multiarch canônico do
  NXExtract. A arquitetura do `nxport.json` seleciona exatamente uma linha e o
  gate confere path, modo, tamanho, SHA-256, GLIBC, classe e machine ELF.
- Registra arquitetura, teto GLIBC e hashes do manifesto/fonte da UI nos
  metadados internos e revalida tudo no stage e no ZIP reaberto.
- Exige a atestação gráfica restaurada do NXExtract e preserva, sem alteração
  visual, a NXSplash obrigatória de cinco segundos.
- Mantém todos os gates 0.2.10 de allowlist, shell, PortMaster, dependências,
  baixa GLIBC, SBOM, determinismo, publicação atômica e reabertura do ZIP.

## 0.2.10 — 2026-08-14

- Adota nxbootstrap 0.6.12 e NXExtract 1.2.7 para novos releases opt-in.
- Fixa no auditor os hashes canônicos do engine, runner, helper de ambiente e
  UI; atualizar o próprio manifesto junto de um arquivo adulterado não basta.
- Exige `--require-ui`, atestação SDL/TTY antes do scan e monitoramento do
  renderer durante o setup, mantendo a transação fail-closed.
- Preserva a política flexível por package/ABI e conteúdo crítico e o splash
  NEXTOS/RETRO ELITE de cinco segundos em toda abertura.

## 0.2.9 — 2026-08-14

- Torna `nxextract-ui` um ELF Linux obrigatório, canônico e pinado em todo ZIP
  com NXExtract: AArch64, modo 0755, SHA-256 imutável e GLIBC máxima 2.17.
- Reabre o ZIP final e revalida UI, inventário, SBOM, dependências e ordem
  `NXExtract/UI → payload → nxsplash → adapter → jogo`.
- Rejeita receitas de container presas a um único SHA/tamanho do APK externo;
  exige package e identidade por conteúdo crítico ou duas ou mais variantes
  explícitas, preservando diferenças legítimas de assinatura/empacotamento.
- Mantém nxsplash 0.1.1 obrigatório por cinco segundos em toda abertura e
  conserva os gates de muOS/ROCKNIX/Knulli/ArkOS/TrimUI e ausência de `stat`.

## 0.2.8 — 2026-08-14

- Atualiza a identidade canônica do helper obrigatório para nxsplash 0.1.1 e
  seus novos artefatos imutáveis por arquitetura.
- Registra nos metadados/SBOM do ZIP o helper que preserva o backend SDL e usa
  apenas o VT ativo publicado pelo kernel como fallback de console.
- Mantém todos os gates de ELF, GLIBC, ordem, modo, hash, dependências e
  reabertura do ZIP; releases antigas permanecem presas ao NXRelease anterior.

## 0.2.7 — 2026-08-14

- Exige que releases com `nxbootstrap 0.6.9+` incluam exatamente um
  `nxsplash-nextos` da ABI, modo 0755 e bytes idênticos ao release imutável do
  componente.
- Adiciona o kind `nxsplash-linux` e submete o helper aos mesmos gates de ELF,
  GLIBC baixa, `DT_NEEDED`, inventário, SBOM, stage e ZIP final.
- Registra versão, ABI, duração, manifesto, fonte e SHA-256 do nxsplash nos
  metadados internos revalidados.
- Prova a ordem `NXExtract → payload → nxsplash → adapter/bibliotecas → jogo` e
  rejeita remoção, troca, reclassificação, reordenação ou chave de skip.
- Corrige o JSON Schema v2 para aceitar o launcher autocontido único que o
  runtime já exige desde nxbootstrap 0.6.0.

Releases históricas continuam pinadas às versões anteriores; nenhuma árvore de
port é migrada automaticamente.

# 0.3.10 (2026-08-30, gate de controles editáveis + nxgenerator 0.3.8)

- Exige nxgenerator 0.3.8. Aceita 0.3.9 como versão histórica reproduzível.
- Gate TEARSCAPE-CONTROLS-LIVE no nx-render-manifest: port com
  `controls.runtime_mapping = nxinput-gptk` precisa empacotar um executável
  que realmente linka o runtime GPTK do nxinput (marcador
  `nxinput-gptk-runtime/1`) e um NEXTOSCONTROLLERS.gptk default com magic
  válido; port SEM a declaração não pode embarcar um arquivo de mapping que
  se declara editável ("This file is YOURS"/"Este arquivo é SEU"). ZIPs já
  publicados não são tocados; a fronteira vale para cada candidato novo.
