# NXRelease

## Fluxo V5 simples (0.4.11)

A composição corrente exige nxbootstrap 0.8.4 e nxgenerator 0.4.5. O primeiro
preserva um `$LIB`/`${LIB}` herdado realmente mapeado e remove do filho apenas
o SDL de sistema herdado que a própria `ld.so` não conseguiu carregar; o
segundo fixa no receipt a versão e os hashes exatos dos sources do bootstrap.

`stage`, `build` e `bundle` usam autoridade humana quando nenhum candidate lock é fornecido.
Isso significa que a palavra do dono decide se gameplay, vídeo e controles foram aprovados; o
NXRelease não exige nem fabrica receipts de aparelho. A ferramenta continua fechando os fatos
objetivos do pacote (hashes, inventário, SBOM, GLIBC, privacidade, licença, instalação e layout
PortMaster) e reabre o ZIP final uma única vez.

```sh
python3 framework/nxrelease/nxrelease.py bundle \
  --authority human --manifest nxrelease.json --stage /caminho/stage \
  --destination /caminho/release --archive-name jogo.zip
```

O fluxo antigo continua disponível como opt-in com `--authority candidate-lock
--candidate-lock <arquivo>`. `public-final` é uma fronteira legada e não faz parte do fluxo
humano simples.

Versão atual: **0.3.25**.

A 0.3.25 liga o scanner estrutural `nxscan.py` ao fluxo real: para `.py`,
`.pyi` e `.json` a gramática (AST/JSON estrito) decide antes do fallback
textual, e um `FAIL` estrutural rejeita na hora. O adaptador é
**estritamente aditivo**: `PASS` nunca absolve e `UNSUPPORTED`/
`STRUCTURAL_ERROR` voltam para a autoridade regex fail-closed, que
permanece mais ampla. O scanner de nomes privados não foi enfraquecido.

A 0.3.23 (V4-05A) adiciona a fronteira agregada SOMENTE LEITURA
`nxrelease preflight` — todas as categorias avaliadas em ordem
determinística, todos os erros independentes juntos, receipt
`org.nextos.v4.preflight-receipt/1` vinculado a commit/tree e aceito pelo
nxledger, e o `public-final` recusando qualquer receipt DEV/FAIL/stale antes
da primeira mutação — e fecha a paridade integral do piso SDL: decisão única
`nxabi.decide_sdl_floor()` nos dois consumidores, símbolo desconhecido
fail-closed para candidato público, alcance a consumidores indiretos e
waiver histórico sem poder de rebaixar veredito público.

A 0.3.22 fecha a política única de piso SDL (V4-03B). A decisão sobre todo
import direto `SDL_*` de ELF que declara NEEDED da SDL2 core é por versão de
nascimento, lida da autoridade única e versionada
`framework/nxabi/sdl2-symbol-floor.tsv` através do próprio parser estrito do
nxabi — os dois consumidores abrem exatamente os mesmos bytes e registram o
mesmo id + SHA-256 nos recibos. Import nascido acima do piso universal SDL
2.0.4 (caso de campo `SDL_JoystickGetVendor`/`SDL_JoystickGetProduct`, SDL
2.0.6) ou ausente da autoridade reprova no `validate` — a primeira fronteira
somente leitura, antes de stage, cópia, build ou ZIP — nomeando ELF, símbolo,
versão exigida, piso declarado e hash da autoridade, sem waiver. A lista
paralela `symbol-floors/libSDL2-2.0.so.0.syms` foi removida; se reaparecer,
falha fechado. A rota canônica para APIs pós-piso é o resolver opcional do
nxcompat 0.4.0, sem import direto; SDL empacotada e as demais famílias de
piso preservam a semântica anterior.

A 0.3.21 fixa a composição corrente no nxinput 0.9.0. A normalização de
domínio PortMaster/joydev — ativada somente quando o bitset do event node e os
bindings de volume provam positivamente o dialeto joydev antigo — passa a ser
a identidade de nxinput exigida pela composição, sem mudar sintaxe, ordem de
autoridade ou qualquer semântica fail-closed do release. GUID duplicado
divergente continua seguindo a semântica da SDL: a última linha vence e a
tolerância fica registrada no receipt. Nenhum outro contrato, interface
visual, geração ou byte de port muda.

A 0.3.20 alinha a validação host-side da receita ao NXExtract 1.3.0. O engine
interpreta a ausência do campo top-level `validate` exatamente como uma lista
vazia, portanto ambas as formas são aceitas. `extract` e `commit` permanecem
arrays obrigatórios, e um `validate` presente com qualquer outro tipo continua
recusado. Nenhum outro contrato, interface visual, geração ou byte de port
muda.

A 0.3.19 corrige a classificação de fonte Python anotada no scanner de
privacidade. Uma declaração gerada como
`m_VCPassword: Optional[str] = None` descreve um tipo e não contém uma senha;
ela deixa de ser confundida com `key: value`. Atribuições reais como
`password=abcdefgh`, inclusive com uma anotação antes do `=`, continuam
falhando fechadas. Formatos não Python preservam a proteção anterior para
literais `key: value`. A composição continua nxbootstrap 0.7.8 e nxgenerator
0.3.12, sem mudança visual, de geração ou de port.

A 0.3.18 fixa a composição corrente em nxbootstrap 0.7.8 e nxgenerator
0.3.12. O gerador agora distingue no próprio cabeçalho do
`NEXTOSCONTROLLERS.gptk` um mapa nativo estático de um mapa realmente vivo:
sem `controls.runtime_mapping`, o arquivo avisa que edições não chegam à
engine; com `controls.runtime_mapping = "nxinput-gptk"`, preserva a promessa
editável e todas as provas de runtime já exigidas. O release mantém o gate
fail-closed: um arquivo que prometa edição sem runtime continua recusado.

A 0.3.17 fecha a provenance da closure estática de hooks: nenhum arquivo
alcançável pelo hook pode conter um digest literal de 64 hexadecimais. Nem uma
chave arbitrária como `output_sha256`, nem o mesmo valor autenticado em
`patch_profiles` autorizam o digest a reaparecer no código ou numa spec. Dados
JSON pequenos e UTF-8 estritos recebem a mesma decisão com `.json` ou outro
sufixo; ELF, NUL, binário, texto inválido e JSON escalar não são promovidos a
closure textual. A composição histórica permanece nxbootstrap 0.7.8 e
nxgenerator 0.3.11.

A 0.3.16 fixa nxbootstrap 0.7.8 mantendo nxgenerator 0.3.11. Uma biblioteca
Linux privada usada por um helper NXExtract permanece no papel
`private-library`, fica abaixo da raiz privada declarada em `nxextract/` e é
renderizada como `third-party-linux` com modo `0644` e SHA-256 exatos. O
`public-final` inclui o mesmo membro na closure viva do NXExtract. Isso não
autoriza ELF de dados nem helper 0644: um `nxextract-helper` ELF continua sendo
`project-linux` executável `0755`.

A 0.3.15 fecha duas classificações falsas sem abrir um bypass genérico. Um
ELF Linux declarado em schema 3 como `nxextract-helper` entra como
`project-linux`, com modo executável, metadados ELF completos, teto de ABI/GLIBC
e reprodução no `public-final`; `nxextract-spec`, `runtime-data` e
`runtime-hook` continuam dados e recusam qualquer ELF. Na evidência física,
somente o opt-in literal `controls.runtime_mapping = "nxinput-gptk"` exige o
bloco GPTK. Um port que preserva ações/contextos apenas para descrever o input
nativo usa `integrations.input = null`; inventar um bloco GPTK nesse caso falha
fechado. Todas as exigências GPTK da 0.3.14 permanecem quando o opt-in existe.

A 0.3.14 fixa a composição corrente em nxbootstrap 0.7.7 e nxgenerator
0.3.11. O release só aceita launchers cuja promoção de geração exige status 0
do filho além dos receipts exatos de health e vídeo. A 0.3.13 permanece
reproduzível somente por autoridade histórica externa e nunca vira candidata
corrente por metadata. No opt-in Godot, a composição corrente exige ainda o
marcador `nxgl-godot-frame-proof/2` e os símbolos definidos
`nxgl_frame_proof_is_fatal`/`nxgl_frame_proof_consume_fatal`; o glue antigo,
que apenas publicava o receipt e deixava o present continuar, não passa. O
gerador pinado também exige que uma promoção preserve literalmente o
`input_controller_profiles` declarado, impedindo perder `controllers.nxb`.

A 0.3.13 acrescenta a identidade Godot à prova externa sem relaxar o GPTK:
quando declarados, `nxinput-godot-runtime/1` e
`nxgl-godot-frame-proof/1` formavam o par histórico indivisível e precisavam existir nos
bytes do ELF candidato. Ports sem esse opt-in mantêm a prova 0.3.12 literal.

A 0.3.12 torna o GPTK editável uma integração comprovada de ponta a ponta.
`nxinput-gptk-runtime/3` e os símbolos definidos de `nxinput_gptk_live`
substituem o marker textual v1; o adapter promovido precisa começar em contexto
não provado e deixar o evento seguir nativo enquanto contexto e sinks não
estiverem prontos. O mesmo `candidate-lock` externo carrega
`input_proof`/`input_proof_receipt_sha256`, ligados ao ELF, geração, default e
adapter exatos, com um caso evento → ACTION → sink → ACK para cada binding.
Load sem dispatch, contexto implícito, sink inexistente ou ACK falho não podem
produzir candidato verde.

A 0.3.13 fixa a composição corrente em nxbootstrap 0.7.6 e nxgenerator 0.3.10,
mantendo integralmente o gate de controles editáveis da 0.3.10, inclusive o
nome canônico `controllers.nxb` usado pela declaração NXC6 no runtime. O auditor
reconhece a fronteira supervisionada real de `sdl_provider=system` introduzida
no nxbootstrap 0.7.5+: o filho é localizado pelo guard
`NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD`, e a restauração de um
`SDL_DYNAMIC_API` já pertencente ao firmware/adapter por uma variável
`NXBOOTSTRAP_SYSTEM_SDL_*` não é confundida com redirecionamento privado.
Qualquer SDL embarcada ou redirecionamento fora dessa fronteira continua
falhando fechado.

A 0.3.10 adotou nxgenerator 0.3.8 e fechou o contrato
`controls.runtime_mapping = nxinput-gptk`: o executável precisa conter o
marcador do runtime e o default GPTK editável precisa ser real e parseável.

A 0.3.9 torna o seed V4 uma invariável semântica do core, e não apenas um
arquivo que o renderer costuma incluir. Toda generation-v2 publicável carrega
exatamente `<port-id>/nxruntime-<generation-id>.nxb`, arquivo regular `0644`
classificado como `nxruntime-seed`. O header `NXBUNDLE1` precisa nomear o mesmo
port e a mesma geração autenticados pelo pacote, e seus membros precisam ser a
closure exata do store `.nxruntime/generations/<generation-id>/`, preservando
path, modo, tamanho e SHA-256, com uma única exclusão deliberada: `commit` é o
recibo local escrito por último e jamais entra no seed.

Essa relação é recalculada sobre os bytes de source, do stage e do ZIP
reaberto. Seed ausente, renomeado, duplicado, estrangeiro, stale, com membro a
mais/menos ou divergente não pode ser encoberto por metadata ou
`GENERATION.json` coordenados. O launcher continua sendo comparado ao render
canônico byte a byte do nxbootstrap 0.7.4; portanto ele contém o caminho de
materialização que reconstrói a geração quando toda `.nxruntime` foi perdida.
A composição continua nxbootstrap 0.7.4 + nxgenerator 0.3.7.

Versões anteriores, inclusive a 0.3.8, preservam seus bytes somente pelo fluxo
histórico read-only com autoridade externa, SHA exato e path de quarentena.
Elas não herdam a política nova por metadata, não são migradas implicitamente
e continuam inelegíveis para `public-final` corrente.

A 0.3.8 fecha duas fronteiras que antes eram apenas declarativas. Release
público usa SDL1/SDL2 do sistema/PortMaster e recusa qualquer cópia privada;
SDL3 privada só existe por uma exceção completa ligada a ELF Linux real,
ABI/SONAME/mode/path/version/source/licença/razão e SHA exatos. Nenhum
stage/ZIP novo nasce sem um `nxrelease-candidate-lock-v1` externo, aberto por
fd `O_NOFOLLOW`, read-only, sem hardlink e fora do source, ligando o candidato
aos bytes de source, stage e ZIP reaberto. Ports com
`video_proof: "required"` ligam ainda o receipt físico `OK/non-black` à mesma
geração e ao mesmo executável. A composição exigida é nxbootstrap 0.7.4 e
nxgenerator 0.3.7.

O fluxo normal compila e empacota uma vez. `public-final` faz uma única
compilação limpa para conferir o candidato já testado; `build`/`bundle` não
repetem stage/package por padrão. `--prove-deterministic` é somente um
restage/repackage diagnóstico explícito dos mesmos inputs, nunca outra
compilação do projeto. Archives 0.2.40–0.3.19 só podem ser lidos com autoridade
externa e SHA exato em caminho claramente nomeado `quarantine`; esse modo
imprime `publication_eligible=false` e nunca entra em `public-final`.

A 0.3.7 é a composição integrada na V4 aberta: exige nxbootstrap 0.7.3
(runtime-data + rollback real-crash) e nxgenerator 0.3.6 (runtime gerenciado +
catálogo do site). Nenhum gate muda; candidatos 0.3.6 autenticados continuam
reproduzíveis.

A 0.3.6 integra nxbootstrap 0.7.2, nxgenerator 0.3.5 e PortMaster 2.1.1.
O renderer reconhece `runtime-data` como payload não ELF autenticado na árvore
ativa e no store generation-v2; path, modo `0644`, SHA-256 e fechamento de
`GENERATION.json` continuam obrigatórios.

A 0.3.5 reconhece `assemblyVersion` e `fileVersion` de quatro partes em
`*.deps.json` como metadados estruturados do runtime .NET, sem relaxar o gate
para IP em qualquer outro campo ou tipo de documento.

A 0.3.4 adota o nxgenerator 0.3.4 e o contrato PortMaster v3. Em metadado v4,
uma lista de runtimes vazia passa a ter uma única forma canônica: o campo
`attr.runtime` fica ausente. Se presente, ele precisa ser uma lista não vazia,
única e de strings. Isso mantém o HarbourMaster atual verde e evita o crash do
PortMaster oficial legado `2024.03.10-0841`.

A 0.3.3 trata o seed visível `NXBUNDLE1` como o contêiner autenticado que ele
é: valida índice, offsets, tamanho e SHA-256, e aplica o scanner de privacidade
a cada membro isoladamente. Um ELF deixa de herdar a classificação textual do
cabeçalho ASCII e constantes binárias de protocolo não viram falsos positivos;
texto com IP de device, credencial, hostname ou path privado continua recusado.

A 0.3.2 fecha o bundle `NXCONTROLLER_PROFILES/1` e o contrato
`NEXTOSCONTROLLERS/2`, incluindo validação contra os bytes reais e contra os
sinks do adapter.

A 0.3.1 arma o gate da fronteira `post-first-present` (V4-GRAPHICS-04): com o
opt-in declarativo no contrato do port, a prova gráfica física só promove com
receipt emitido depois do primeiro present real do guest
(`phase=post-first-present`, `first_present=1`, `pre_drawable`), one-shot e com
identidade de commit consistente. Sem o opt-in, o gate anterior permanece
literal.

A 0.3.0 tornou historicamente a dupla prova de reprodutibilidade obrigatória
no fluxo canônico; a 0.3.8 a substitui pela barreira de lock/stage/reopen e
mantém o segundo restage somente como diagnóstico explícito. Ela compara todos
os ELFs autenticados além do ZIP, nomeia o
ELF divergente quando falha e emite `BUILD-PROVENANCE.json` sanitizado. Também
valida os opt-ins declarativos V4 e recusa `DT_NEEDED libEGL` em qualquer ELF
de um pacote cujo `egl_binding` esteja ligado.

Versão anterior: **0.2.43**. Ela exige nxbootstrap 0.6.37, NXExtract 1.2.21 e
nxgenerator 0.2.20 exato. Assim, um candidato real com geração v2 compõe, antes do
recibo, tanto a closure NXExtract já materializada quanto o `package_payload`
autoral declarado e o tuning GPTK do port. O renderer confere cada path, kind,
modo e SHA-256 contra o inventário final; não existe overlay posterior nem
queda silenciosa para versão `0.0.0`. O recibo liga também o hash do projeto e
a closure exata de artefatos, impedindo composição coordenada depois da
geração. Ela preserva a reabertura autenticada da
0.2.36 e fecha a
promoção física: `public-final` exige o evento canônico `NXU0006` ligado ao
mesmo run e generation do ZIP testado. Assim uma
geração válida com `defaults/NEXTOSCONTROLLERS.gptk` pinado percorre
`stage`, `build` e `bundle` sem falso negativo, enquanto um pin ausente ou
divergente continua falhando antes da publicação. Os comandos
históricos continuam preserváveis como evidência, mas desde 0.3.8 somente no
modo explícito de quarentena read-only. Apenas `public-final` sob todos os pins
correntes pode emitir promoção final ligada a receipts físicos.

O nxbootstrap 0.6.37 corrige exclusivamente modos `0644`/`0755` alterados por
uma normalização recursiva do PortMaster em mídia POSIX: primeiro autentica a
closure inteira sem confiar nos bits, depois restaura os modos declarados e
repete a validação fechada. Controles transacionais não são reconstruídos;
uma geração sem `commit` continua truncada e é recusada.

No store imutável generation-v2, controles e payloads não ELF mantêm o kind
`nxruntime-generation`. Cópias ELF usam `nxruntime-generation-linux` e carregam
exatamente arquitetura, build profile, `DT_NEEDED`, `SONAME` e proveniência do
membro live correspondente. Ambas as cópias passam pelo teto GLIBC e qualquer
diferença de path lógico, modo, SHA-256 ou metadado falha antes de `stage`.
Bibliotecas privadas declaradas como `0644` preservam esse modo autenticado já
na primeira classificação de `lib/*.so*`; o renderer não inventa permissão de
execução, e também não aceita drift de modo entre a cópia live e o store.

Em schema 3, `nx-refresh-pins.py` também recalcula os hashes declarados em
`package_payload`. O campo só transporta arquivos regulares, explícitos e
autenticados do próprio port; executáveis/runtime, dados do dono e namespaces
gerados continuam sob suas autoridades específicas. `README.md` e
`INSTALLATION.md` autorais substituem apenas os scaffolds correspondentes e já
entram nos `artifacts` de `GENERATION.json` com seus bytes definitivos.

Esta versão fecha o contrato PortMaster v4 com o HarbourMaster real fixado pelo
framework. `port.json` e `INSTALLATION.md` bilíngue são obrigatórios; `items`,
`items_opt`, `runtime`, `arch`, `min_glibc` e `title` precisam descrever os
bytes reais do ZIP. O parser JSON é estrito tanto no manifesto quanto nos
metadados reabertos: BOM, chaves duplicadas, `NaN`, truncamento e vírgula final
falham. O comando `verify --previous-archive` executa também o ciclo offline
install → discovery → restart → update → uninstall → reinstall sobre o ZIP
novo, usando a cópia imutável do PortMaster-GUI registrada pelo framework.
Nesse overlay, uma geração anterior completa e autenticada é preservada de
propósito: v2 como âncora de rollback A/B e v1 histórica como control-only inerte.
O gate permite exclusivamente essa closure
sob `.nxruntime/generations/<id>/`, byte por byte conforme o `GENERATION.json`
anterior; qualquer outro membro aposentado continua fatal.

Além desse contrato, o auditor continua fixando os hashes do engine, runner e
helper de ambiente do engine NXExtract 1.2.21. A UI gráfica preservada continua
na versão visual 1.2.16 e é selecionada por arquitetura em
`ui/release/manifest-v1.json` e validada por SHA-256, tamanho, modo, GLIBC,
classe e machine ELF. Um manifesto não pode se autoatestar depois de trocar o
runner: ausência de `--require-ui`, prova de renderer ou monitor de vida falha
antes de construir o ZIP.

Esta versão fecha releases novos com nxbootstrap 0.6.37, engine NXExtract
1.2.21, NXExtract UI 1.2.16 multiarch e nxsplash 0.1.2. A UI é obrigatória
durante a extração; a logo NEXTOS continua
obrigatória por cinco segundos em toda abertura. Ports históricos continuam
presos ao NXRelease e aos helpers que aprovaram; não existe migração automática.

NXRelease é o gate host-side para um ZIP público/multi-device do PortMaster. Ele
recebe um manifesto explícito, copia somente os arquivos declarados para um
stage novo, audita o conteúdo, produz um ZIP determinístico e então abre o ZIP
gerado novamente e repete a verificação. Ele não executa jogo, APK ou loader.
Build e verify também chamam o auditor independente
`framework/tests/audit-portmaster-zip.py` sobre o arquivo ZIP real. Todo shell
allowlisted — inclusive helper sem extensão reconhecido por shebang — passa por
sintaxe e recusa do comando externo `stat`; `nxbootstrap.sh` aposentado falha.

O teto público de GLIBC está gravado no código como `2.30`. O manifesto ou a
linha de comando podem torná-lo mais estrito (por exemplo, `2.17`), nunca mais
permissivo. Todos os ELFs do pacote precisam ser classificados; um ELF Linux
feito pelo projeto ou fornecido por terceiro passa pelo mesmo gate de símbolos
versionados. ELF Android original/BYO, APK, OBB e dex nunca entram no ZIP
público: ficam nos dados fornecidos e extraídos pelo próprio dono.

## Contrato do pacote

O manifesto JSON segue [schema/nxrelease-v2.schema.json](schema/nxrelease-v2.schema.json).
O schema v2 é deliberadamente incompatível com o v1: o v1 não fechava
`DT_NEEDED`, não pinava o helper/receita completos do NXExtract e permitia uma
cadeia de launcher apenas lexical. O gate rejeita v1 em vez de presumir
defaults inseguros.
Os campos essenciais são:

- `package.profile`: sempre `universal-portmaster`;
- `package.launcher`: um único `.sh` na raiz do ZIP, modo `0755`;
- `package.launcher_chain`: no `nxbootstrap 0.6.x`, exatamente o launcher
  autocontido visível; contratos históricos 0.5.x usavam também uma biblioteca
  versionada. O gate valida bytes, configuração, call graph e ausência dos
  artefatos aposentados `nxbootstrap.sh`, `nxdeployment.json` e `run.sh`;
- `package.launcher_contract`: versão do nxbootstrap e pin de
  `<port>/nxport.json`;
- `package.port_dir`: a pasta do port na raiz;
- `release.source_date_epoch`: timestamp fixo entre 1980 e 2107;
- `release.max_glibc`: opcional, padrão `2.30`;
- `nxextract`: versão e pins do layout canônico inteiro: `extractor.json`,
  `nxextract.py`, `run-extractor.sh` e `nxextract-runtime-env.sh`;
- `dependencies`: provedor explícito para cada chave
  `(namespace, architecture, DT_NEEDED)`;
- `portmaster_metadata`: pin obrigatório de `port.json`; `gameinfo.xml` e
  imagens permanecem opcionais, mas sempre pinados quando presentes;
- `files`: fontes e destinos, sem glob implícito e sem caminhos absolutos.

Durante o audit, o NXRelease acrescenta ao receipt interno do NXExtract
`ui_architecture`, `ui_glibc_max`, `ui_release_manifest_sha256`,
`ui_source_sha256` e `ui_version`. Esses campos são derivados do manifesto multiarch canônico,
não são aceitos como alegação do manifesto de entrada, e voltam a ser conferidos
ao reabrir o stage e o ZIP final.

O `nxport.json` pinado precisa ser a saída canônica do schema v2. O gate confere o
objeto NXExtract `1.2.21`, paths privados, capabilities presentes exatamente no
registry finito `framework/nxcompat/capabilities-v1.json`, quirks, relatório e cada
assignment correspondente no launcher visível. Capabilities apenas sintaticamente
válidas não são aceitas; quirks ainda são validados por namespace e sintaxe e devem
permanecer específicos do adapter/jogo até existir um registry finito. Entrada
nxport v1 deve passar primeiro pelo
gerador; o release não completa defaults legados por conta própria.

O campo opcional `execution_roles` é o único opt-in mixed-ABI. O NXRelease
seleciona e audita separadamente o ELF do extrator, splash, jogo e helpers,
incluindo ABI, classe, machine, `PT_INTERP` e closure. Dependências de papéis
`host` ou `firmware` não podem vir do diretório privado do port. Sem esse campo,
o caminho single-ABI anterior permanece inalterado. Se `GENERATION.json` estiver
no pacote, seus papéis devem coincidir exatamente com os de `nxport.json`.

Desde `nxbootstrap 0.6.9`, `required_files[1]` é obrigatoriamente
`nxsplash-nextos`. O NXRelease exige exatamente um
`<port>/nxsplash-nextos`, classificado como `nxsplash-linux`, modo `0755`, ABI e
SHA-256 idênticos ao release imutável do componente. Ele também comprova que o
launcher executa a tela fixa de cinco segundos depois do payload e antes do
adapter, bibliotecas privadas e lifecycle nativo, sem chave de remoção por port.

O ELF AArch64 NXSplash 0.1.2 atual, SHA-256
`d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97`,
continua `physical-unverified` até o mesmo ZIP Hitman GO ser aceito nos dois
aparelhos autorizados. Pin e golden host não substituem essa prova.

Os receipts físicos isolados da UI NXExtract 1.2.9 também têm escopo limitado. No
NextOS/Mali-450 houve `visible=sdl` via `mali` e captura gráfica válida; no alvo
ArkOS-class/KMSDRM houve `visible=sdl`, log KMSDRM e recuperação EGL/GLES, mas
nenhuma captura visual válida. Os processos foram encerrados nos dois alvos.
Esses receipts não atestam instalação nem jogo completo. ROCKNIX/Panfrost segue
com UI física não verificada: o histórico TASM2 extraiu mais de 1 GiB, porém a
interface ficou preta.

O campo opcional `language` só é aceito para adapters que declaram `default` e
uma lista finita `supported`. Nesses ports, o launcher canônico expõe a linha
editável `GAME_LANGUAGE`, valida o código e entrega `NXPORT_LANGUAGE` ao adapter.
Sem o campo, nenhuma opção de idioma aparece. O locale Linux `LANG` nunca é
alterado pelo framework.

Uma entrada de diretório é aceita somente para `payload`. Scripts são sempre
arquivos `.sh` individuais e auditáveis. Se houver
um ELF dentro dela, o build falha como “unclassified”; isto é intencional. Cada
ELF deve ter uma entrada de arquivo própria, SHA-256, `DT_NEEDED` exato e
`DT_SONAME` esperado (string ou `null`) com um destes tipos:

- `project-linux`: ELF construído pelo port;
- `third-party-linux`: runtime Linux redistribuído, com proveniência.
- `nxsplash-linux`: helper do framework, conferido byte a byte contra o release
  canônico da ABI.

Todo ELF precisa ser realmente carregável: `ET_EXEC`/`ET_DYN`, little-endian,
ao menos um `PT_LOAD`, classe coerente com a ABI e, em ARM Linux, EABI5
hard-float. `PT_INTERP` Linux, quando presente, é exatamente
`/lib/ld-linux-aarch64.so.1` ou `/lib/ld-linux-armhf.so.3`. RPATH/RUNPATH é
proibido no perfil universal. Falha em qualquer fase relevante de `readelf`
encerra o gate.

Cada `DT_NEEDED` deve aparecer uma única vez em `dependencies`, separado entre
`linux` e `android` e por ABI. Os provedores possíveis são `package`,
`glibc-base`, `firmware`, `portmaster` e `nxloader-import-registry`. Um provider
`package` aponta para o ELF com aquele `DT_SONAME`; providers duplicados ou
SONAMEs não portáteis são rejeitados. `libstdc++.so.6` e `libgcc_s.so.1` não são
implicitamente parte de `glibc-base`: precisam de contrato real de firmware,
PortMaster ou pacote.

Os três tipos Linux exigem `architecture`, `provenance` e
`build_profile: "universal-low-glibc"`. Assim uma variante construída contra a
glibc atual não pode ser misturada silenciosamente no ZIP universal, inclusive
quando for estática e não expuser símbolos `GLIBC_*`.

Fonte e licença são obrigatórias no perfil público: o kind `license-notice`
registra um arquivo `LICENSE`/`NOTICE`/
`COPYING` pinado por SHA-256 (modo `0644`), e `package.license` descreve
`spdx_id`, `source_url` da fonte pública e o `file` amarrado. O gate recusa
URLs/fontes que contenham path pessoal ou IP literal e rejeita o manifesto se o
mapa de licença estiver ausente.

O launcher de raiz 0.6.x contém toda a configuração declarativa e o runtime do
bootstrap. NXRelease confere a renderização canônica, o `nxport.json` pinado, a
ordem NXExtract → payload → nxsplash → adapter/bibliotecas → processo do jogo,
o destino de `exec` e as funções efetivamente alcançadas que integram
`control.txt`, `get_controls`, `pm_platform_helper` e `pm_finish`. Todos os
arquivos classificados como `script` passam por sintaxe e auditoria; inclusive
um background escondido como `cmd & ;;` é detectado. Drivers SDL/OpenAL fixados,
processos soltos em background, `setsid`/`nohup` e gerenciamento direto do
frontend são bloqueados. As únicas exceções são `adaptive-driver` (retry depois
de uma falha real) e `supervised-child` (PID/trap/wait); cada exceção precisa
nomear o script exato e trazer uma justificativa concreta no manifesto.

`portmaster_metadata` e seu `port_json` são obrigatórios. O documento precisa
ser schema v4, e `items` + `items_opt` formam exatamente o inventário de topo do
ZIP, com sufixo `/` somente para diretórios e comparação case-sensitive. O
launcher e a pasta do port pertencem a `items`; `attr.runtime` fica ausente
quando vazio e, se presente, é uma lista explícita não vazia; `attr.arch`
coincide com todos os ELFs Linux empacotados,
`attr.min_glibc` não subdeclara o maior requisito e `attr.title` coincide com
`nxport.json`. `gameinfo.xml` e imagens são opcionais, mas recebem pin SHA-256 e
validação de paths/magic quando presentes. Os tipos correspondentes em `files`
são `portmaster-metadata` e `portmaster-image`.

Todo manifesto inclui ainda `<port-id>/INSTALLATION.md` como `payload`, modo
`0644` e SHA-256 explícito. O arquivo precisa ter seções `## English` e
`## Português`; a auditoria é repetida na fonte, no stage e no ZIP reaberto.
Para ports Android, o conteúdo registra nome/versão do jogo, package ID, ABI,
tamanho e SHA-256 do APK de referência, sem revelar origem de download. O gate
geral de dados privados/proibidos também percorre esse documento.

## Exemplo mínimo de manifesto

```json
{
  "schema_version": 2,
  "source_root": ".",
  "package": {
    "id": "meujogo",
    "version": "1.0.0",
    "profile": "universal-portmaster",
    "launcher": "Meu Jogo.sh",
    "launcher_chain": ["Meu Jogo.sh"],
    "launcher_contract": {
      "generator": "nxbootstrap",
      "version": "0.6.15",
      "config_path": "meujogo/nxport.json",
      "config_sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    "port_dir": "meujogo",
    "license": {
      "spdx_id": "GPL-3.0-only",
      "source_url": "https://example.org/meujogo-source",
      "file": "meujogo/LICENSE"
    }
  },
  "release": {
    "source_date_epoch": 1785542400,
    "max_glibc": "2.30",
    "compression": "deflated"
  },
  "nxextract": {
    "path": "meujogo/nxextract/nxextract.py",
    "version": "1.2.10",
    "minimum_version": "1.2.2",
    "sha256": "b1b46ecdf1336b1412d7d3a3d291220aca4834a47730a5545afb382dae6036b5",
    "runner_path": "meujogo/nxextract/run-extractor.sh",
    "runner_sha256": "c931427c7226d22d7e30eee8549b50f0621dca1c9d0336634aca08631f454d7a",
    "runtime_env_path": "meujogo/nxextract/nxextract-runtime-env.sh",
    "runtime_env_sha256": "332919a9960d4317563b647f9932d1a4367da147a425fe2f78eafd706f01563f",
    "ui_path": "meujogo/nxextract/nxextract-ui",
    "ui_sha256": "7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56",
    "recipe_path": "meujogo/extractor.json",
    "recipe_sha256": "COLOQUE_AQUI_O_SHA256_REAL"
  },
  "portmaster_metadata": {
    "port_json": {
      "path": "meujogo/port.json",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    "gameinfo_xml": null,
    "images": []
  },
  "dependencies": [
    {
      "namespace": "linux",
      "architecture": "aarch64",
      "soname": "libSDL2-2.0.so.0",
      "provider": "portmaster"
    },
    {
      "namespace": "linux",
      "architecture": "aarch64",
      "soname": "libc.so.6",
      "provider": "glibc-base"
    },
    {
      "namespace": "linux",
      "architecture": "aarch64",
      "soname": "libdl.so.2",
      "provider": "glibc-base"
    }
  ],
  "files": [
    {
      "source": "Meu Jogo.sh",
      "target": "Meu Jogo.sh",
      "kind": "launcher",
      "mode": "0755",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "nxport.json",
      "target": "meujogo/nxport.json",
      "kind": "nxbootstrap-config",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "INSTALLATION.md",
      "target": "meujogo/INSTALLATION.md",
      "kind": "payload",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "port.json",
      "target": "meujogo/port.json",
      "kind": "portmaster-metadata",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "nxsplash-nextos",
      "target": "meujogo/nxsplash-nextos",
      "kind": "nxsplash-linux",
      "mode": "0755",
      "architecture": "aarch64",
      "build_profile": "universal-low-glibc",
      "provenance": "nxsplash-v0.1.2 immutable release",
      "sha256": "COLOQUE_AQUI_O_SHA256_CANONICO",
      "needed": ["libc.so.6", "libdl.so.2"],
      "soname": null
    },
    {
      "source": "LICENSE",
      "target": "meujogo/LICENSE",
      "kind": "license-notice",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "build/loader-aarch64",
      "target": "meujogo/bin/aarch64/loader",
      "kind": "project-linux",
      "mode": "0755",
      "architecture": "aarch64",
      "build_profile": "universal-low-glibc",
      "provenance": "build-universal-aarch64.sh em Debian Buster",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL",
      "needed": [
        "libSDL2-2.0.so.0",
        "libc.so.6"
      ],
      "soname": null
    },
    {
      "source": "vendor/nxextract.py",
      "target": "meujogo/nxextract/nxextract.py",
      "kind": "nxextract",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "vendor/run-extractor.sh",
      "target": "meujogo/nxextract/run-extractor.sh",
      "kind": "nxextract-runner",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "vendor/nxextract-runtime-env.sh",
      "target": "meujogo/nxextract/nxextract-runtime-env.sh",
      "kind": "nxextract-runtime-env",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "vendor/nxextract-ui",
      "target": "meujogo/nxextract/nxextract-ui",
      "kind": "nxextract-ui-linux",
      "mode": "0755",
      "architecture": "aarch64",
      "build_profile": "universal-low-glibc",
      "provenance": "NXExtract 1.2.16 canonical AArch64 UI",
      "sha256": "8e4a68ae0a611096d23b04628b4f2e8b5cf34755fe9b71d134bc0d7ea6ccf987",
      "needed": ["libc.so.6", "libdl.so.2"],
      "soname": null
    },
    {
      "source": "extractor.json",
      "target": "meujogo/extractor.json",
      "kind": "nxextract-recipe",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "payload",
      "target": "meujogo/assets",
      "kind": "payload"
    }
  ]
}
```

Os placeholders precisam ser substituídos pelo resultado de `sha256sum`. O
SHA da UI não é placeholder: ele identifica o ELF canônico de GLIBC 2.17.
Receitas que copiam o APK como container não podem fixar um único SHA/CRC ou
tamanho exato externo — nem uma lista dessas identidades. Devem declarar a
família de package, ABI e estrutura/interfaces internas realmente consumidas.
Um SHA interno só pode selecionar `patch_profiles` autenticados, todos com o
mesmo caminho genérico/simbólico para uma build compatível desconhecida; ele
nunca vira whitelist de aceitação. Bounds e magic do container são opcionais.

Hooks são auditados pela closure textual alcançável, não somente pelo arquivo
presente em `hooks[].argv`: scripts chamados, módulos importados e specs JSON
empacotados também entram no gate. O conteúdo JSON é reconhecido de forma
independente do sufixo quando for pequeno, UTF-8 estrito e objeto/array. Nenhum
digest literal de 64 hexadecimais pode existir nessa closure. O NXExtract
autentica `patch_profiles` no planejamento e entrega ao hook somente a
identidade do perfil selecionado; a integridade pós-transformação pertence a
`output_validate`, aos checkpoints canônicos da receita ou à
`generation_runtime`, nunca a uma chave inventada dentro da fonte. `fallback`
é um id portável e não pode significar `none`, `reject`, `fail` ou equivalente.
O validador interno é a autoridade; o JSON Schema serve para autocomplete e
validação inicial no editor.

## Uso

Requisitos: host Linux, Python 3.8 ou mais novo, GNU `readelf`, `bash` e
`/bin/sh`. `stage`/`bundle` exigem `renameat2(RENAME_NOREPLACE)` no filesystem;
o `build` direto também exige hard links no diretório de saída. O gate falha de
forma fechada se o host não puder garantir no-overwrite.

```sh
python3 framework/nxrelease/nxrelease.py validate \
  --manifest ports/meujogo/nxrelease.json

python3 framework/nxrelease/nxrelease.py build \
  --manifest ports/meujogo/nxrelease.json \
  --candidate-lock evidence/meujogo-candidate-lock.json \
  --stage /caminho/local/meujogo-stage \
  --output /caminho/local/meujogo.zip

# Publicação conjunta/crash-atômica recomendada:
python3 framework/nxrelease/nxrelease.py bundle \
  --manifest ports/meujogo/nxrelease.json \
  --candidate-lock evidence/meujogo-candidate-lock.json \
  --stage /caminho/local/meujogo-stage-publicacao \
  --destination /caminho/publico/meujogo-1.0.0 \
  --archive-name meujogo.zip

python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/local/meujogo.zip \
  --sha256-file /caminho/local/meujogo.zip.sha256

# Gate de atualização sobre a release imediatamente anterior:
python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/local/meujogo-1.1.0.zip \
  --sha256-file /caminho/local/meujogo-1.1.0.zip.sha256 \
  --previous-archive /caminho/local/meujogo-1.0.0.zip

# Auditoria somente leitura de bytes antigos, fora do caminho publicável:
python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/quarantine/meujogo-historico.zip \
  --historical-authority evidence/meujogo-historical-authority.json
```

Sem flags extras, `build` e `bundle` criam exatamente um stage e um ZIP. A
opção `--prove-deterministic` faz apenas um restage/repackage temporário para
diagnóstico comparativo dos mesmos ELFs já construídos; ela não chama o build
do port e não é requisito de promoção.

O documento histórico segue
[schema/historical-authority-v1.schema.json](schema/historical-authority-v1.schema.json):
fixa `archive_sha256`, `metadata_tool_version`, `nxbootstrap_version`,
`mode: "historical-read-only"` e `quarantine: true`. Ele também é externo,
read-only e lido pelas mesmas regras seguras do candidate lock.

O lock é uma âncora procedural externa pós-prova, nunca campo de
`nxrelease.json` ou `nxport.json`. Ele prova a continuidade de bytes e do
receipt que recebeu; sozinho não é prova criptográfica de que uma pessoa viu
imagem num display físico. Sua forma mínima é:

```json
{
  "schema": "nxrelease-candidate-lock-v1",
  "schema_version": 1,
  "executable": "meujogo/meujogo-nextos",
  "sha256": "SHA256_DO_EXECUTAVEL_COMPROVADO"
}
```

Congele-o (`chmod 0444`) fora de `source_root`, em diretório do usuário sem
escrita de grupo/outros, sem symlink/hardlink, antes de `stage`, `build`,
`bundle` ou `public-final`. Quando `nxport.video_proof` é `required`, o mesmo documento inclui
em par `video_proof` e `video_proof_receipt_sha256`: o primeiro é o objeto
exato schema `org.nextos.nxruntime.video-proof` v1 (`run_id`, `generation`,
`port_id`, `verdict: "OK"`, `reason: "non-black"`); o segundo é SHA-256 da
linha JSON compacta emitida pelo produtor, exatamente na ordem `schema`,
`schema_version`, `run_id`, `generation`, `port_id`, `verdict`, `reason`, mais
`\n`. O NXRelease
recalcula esse hash e fecha port/generation contra `GENERATION.json`.

Quando `controls.runtime_mapping` é `nxinput-gptk`, o mesmo lock inclui o par
`input_proof`/`input_proof_receipt_sha256`. A forma normativa completa está em
`schema/candidate-lock-v1.schema.json` e uma fixture produtora executável em
`tests/test_gptk_runtime_proof.py`. O hash é SHA-256 do objeto `input_proof`
serializado em JSON UTF-8 com chaves ordenadas, separadores compactos e `\n`
final. `contexts`, `sinks`, `cases`, `runtime.symbols` e `targets` são listas
ordenadas/únicas; `cases` fecha cada binding ACTION × sink do projeto e usa a
mesma fonte do contexto observado. O mapping e o adapter são os artefatos da
única árvore gerada que o harness consumiu — nunca valores derivados somente
do `nxproject` nem um lock criado pelo próprio empacotador.

SDL1/SDL2 privada é sempre proibida, inclusive renomeada, estática ou como
add-on gfx/gpu/sound detectável. Uma SDL3 privada exige simultaneamente o
opt-in `input_sdl3_portmaster`, uma dependência `provider: "package"`, o ELF
`third-party-linux` real e `sdl3_exception` no manifesto:

```json
"sdl3_exception": {
  "architecture": "aarch64",
  "license_file": "meujogo/LICENSE-SDL3.txt",
  "license_spdx": "Zlib",
  "mode": "0644",
  "path": "meujogo/libSDL3.so.0",
  "reason": "Jogo SDL3 nativo requer APIs ausentes nos providers declarados.",
  "sha256": "SHA256_EXATO_DA_DSO",
  "soname": "libSDL3.so.0",
  "source_url": "https://github.com/libsdl-org/SDL/releases/tag/release-3.2.0",
  "version": "3.2.0"
}
```

Path, modo, arquitetura, `DT_SONAME`, símbolos SDL3, proveniência, licença e
SHA são rechecados nos bytes reais. Texto com nome `libSDL3.so` não passa.

## Public-final

`validate`, `stage`, `build`, `bundle` e `verify` continuam aceitando o fluxo de
desenvolvimento/scaffold e não significam suporte físico. O template de pacote
os identifica como `DEV/PACKAGE PASS`. O único resultado final é
`NXRELEASE PUBLIC-FINAL: PASS`, produzido pelo subcomando abaixo.

Antes da primeira release nova, gere e versione o manifesto com a identidade de
geração embutida:

```sh
python3 framework/nxrelease/nx-render-manifest.py \
  --port-dir ports/meujogo \
  --framework-root framework \
  --source-url https://example.org/meujogo-source \
  --public-final
```

Depois de testar fisicamente o ZIP exato, preserve o lock externo original e
prepare uma worktree Git limpa no mesmo commit dos receipts. O caminho do único
rebuild deve ser novo, terminar em `.zip` e ficar fora da worktree:

```sh
python3 framework/nxrelease/nxrelease.py public-final \
  --archive evidence/meujogo-tested.zip \
  --receipt evidence/device-a.public-final.json \
  --receipt evidence/device-b.public-final.json \
  --build-script ports/meujogo/build_universal.sh \
  --candidate-lock evidence/meujogo-candidate-lock.json \
  --source worktrees/meujogo \
  --manifest ports/meujogo/nxrelease.json \
  --build evidence/rebuilt.zip \
  --build-provenance evidence/BUILD-PROVENANCE.json
```

`--build-script` é um executável relativo ao repositório e precisa existir no
commit versionado. O gate abre novamente o `--candidate-lock` original por fd
seguro e exige igualdade integral com sua projeção no ZIP testado; o
`document_sha256` embutido nunca vale como proveniência independente. Então
cria um diretório externo privado e vazio e exporta:

- `NX_PUBLIC_FINAL_REPRO_BUILD=1`;
- `NX_PUBLIC_FINAL_OUTPUT_DIR=<diretório externo>`;
- `SOURCE_DATE_EPOCH=<epoch do nxrelease.json>`, `LC_ALL=C` e `TZ=UTC`.

O script deve escrever em `NX_PUBLIC_FINAL_OUTPUT_DIR` somente os ELFs
`project-linux` finais, preservando o path de `files[].source` relativo a
`source_root`. Exemplo: uma fonte de manifesto `merchantskies-nextos` gera
`$NX_PUBLIC_FINAL_OUTPUT_DIR/merchantskies-nextos`; uma fonte
`bin/helper-nextos` gera `$NX_PUBLIC_FINAL_OUTPUT_DIR/bin/helper-nextos`.
Ausência, arquivo extra, symlink, não-ELF ou hash diferente do manifesto falha.
Antes do build, uma preleitura estreita autoriza somente `source_root`, epoch e
os nomes das saídas `project-linux`; ela não valida nem promove o pacote. Depois
do build, o manifesto completo é carregado e cada campo, hash, ABI, GLIBC,
dependência e membro é validado usando exatamente esses ELFs externos. Assim
uma worktree limpa pode não conter os ELFs ignorados, mas nenhuma saída externa
pode substituir launcher, payload, biblioteca do dono ou outro kind. Input
não-project ausente continua falhando. Se o script também modificar ou criar
arquivo na worktree, a checagem Git posterior falha.
Arquivos ignorados pelo `.gitignore` também contam como sujeira nesse fluxo:
`public-final` exige a worktree sem cache, `.build`, ELF ou qualquer outro
resíduo ignorado antes e depois do build.

O receipt segue
[schema/public-final-receipt-v1.schema.json](schema/public-final-receipt-v1.schema.json).
Ele é sanitizado e identifica exatamente pacote/versão, commit completo, SHA e
tamanho do ZIP, path/SHA/build-id do ELF e generation id. Para cada integração
declarada, ele traz prova runtime e símbolos realmente presentes no ELF:

- lifecycle completo e promoção `pending → active` comprovada pela linha exata
  `UPDATE NXU0006`, com run e generation iguais aos do receipt;
- graphics contract inicializado, `GRAPHICS-EVIDENCE`, shader probe e frame
  medido com pixels não pretos — `BLACK` conclusivo sempre falha;
- GPTK lido, parser/dispatcher e sink do port, A/B observado,
  SELECT+START observado e `delivery_count=1` sem input duplo;
- callbacks de áudio vivos (`not-needed` ou recuperação bem-sucedida);
- saída independente concluída quando o adapter a declara.

Todo device de `graphics.required_devices` precisa de receipt próprio. Todos os
receipts devem concordar com o mesmo ZIP, ELF, generation e commit. O gate cruza
também `nxproject.json`, `nxport.json`, `adapter-contract.json`, claims de
`GENERATION.json` e o manifest sob `.nxruntime/generations/<id>/`.

`public-final` exige `physical_support_proven=true` já fechado no
`nxproject.json`/`GENERATION.json`; estado in-progress ou sem proven support não
é publicável. Os receipts externos exatos corroboram essa alegação e prendem o
artefato/run reais, sem alterar os arquivos do port.

Ao final, o ZIP testado e o único rebuild precisam ter os mesmos bytes,
inventário e ELFs, inclusive o executável do lock. Somente então o gate cria,
sem sobrescrever,
[BUILD-PROVENANCE.json](schema/build-provenance-v1.schema.json), contendo apenas
identidades sanitizadas, commit, hash do documento externo, hashes do
executável/ZIP, inventário, auditoria ELF e hashes dos receipts.

`public-final` recusa `--allow-external-generation` e
`--generation-receipt`: pin/schema/generator/launcher legado nunca é
reapresentado como release corrente. Arquivos 0.2.40–0.3.19 podem ser conferidos
somente com `verify --historical-authority`, autoridade externa de SHA exato e
`quarantine: true`, enquanto o próprio path do ZIP contém `quarantine`. Esse
resultado é read-only, não executa o auditor publicável corrente e imprime
explicitamente `publication_eligible=false`.

`stage`, `output` e `destination` nunca são sobrescritos, inclusive se outro processo criar o
destino entre a validação e a publicação. O ZIP e seu `.sha256` são preparados
e verificados antes da publicação. O comando `build` usa links no-replace, lock
exclusivo e rollback por inode para a dupla no mesmo diretório. Como POSIX não
oferece uma transação de duas entradas, `bundle` é o caminho público mais forte:
monta ZIP + `.sha256` em diretório oculto e faz uma única renomeação
`RENAME_NOREPLACE` do diretório, dando visibilidade conjunta mesmo diante de
queda do processo. Use destinos novos para cada release.
O comando `build` só termina depois de verificar o stage, criar o ZIP, reabri-lo,
conferir CRC, ordem, timestamps, modos, inventário, `MANIFEST.sha256`, NXExtract,
todos os ELFs e o SHA-256 externo. Também existem `stage` e `verify-stage` para
diagnóstico separado. Cada arquivo é hasheado novamente após a cópia e a fonte
é conferida outra vez, fechando a janela entre `validate` e `stage`.

O stage contém três arquivos gerados dentro de
`<port_dir>/.nxrelease/`, sem colisão na raiz compartilhada do PortMaster:

- `<port_dir>/.nxrelease/NXRELEASE-METADATA.json`: inventário, modos, contrato
  de dependências, pins nxbootstrap/nxsplash/NXExtract e relatório de
  todos os ELFs (classe, máquina, flags ABI, `PT_INTERP`, `DT_NEEDED`,
  `DT_SONAME`, GLIBC máxima, proveniência e hash);
- `<port_dir>/.nxrelease/SBOM.cdx.json`: CycloneDX 1.5 determinístico
  (serial e timestamp derivados do `package.id` e do `source_date_epoch`),
  projetado a partir do inventário auditado; cada componente de arquivo traz
  SHA-256, kind, modo e, para ELFs, arquitetura/`glibc_max`/`interpreter`/
  proveniência. `verify` reconfere a cobertura e os hashes contra o inventário;
- `<port_dir>/.nxrelease/MANIFEST.sha256`: SHA-256 de cada arquivo do pacote,
  inclusive os metadados e o SBOM, excluindo apenas ele próprio para evitar
  recursão.

`port.json.items` aceita a convenção PortMaster de exatamente uma `/` final em
diretórios (`"meujogo/"`). O gate remove essa barra para comparar, prova que o
membro é de fato um diretório e rejeita `//` ou uma `/` aplicada a arquivo.

Não publique apenas porque o gate estrutural passou. O ZIP exato precisa ser
instalado de forma virgem, testado nas fases definidas pelo projeto e então
passar em `public-final`; qualquer mudança posterior gera ZIP/hash novos e
invalida os receipts anteriores.

## Fechamento M18

`m18-closure-v1.json` liga M18-001..025 a arquivo/linha, garantia, limite de
escopo e teste. O validador puro confirma que os 25 itens estão fechados, que
Android/BYO não voltou à allowlist pública, que licença é obrigatória e que o
corpus adversarial contém traversal, symlink, colisão Unicode, dados privados,
artefatos Android, tamper, corrida e no-overwrite:

```sh
python3 -B framework/nxrelease/tests/test_m18_closure.py
bash framework/nxrelease/tests/test_nxrelease.sh
```

`closed` aqui significa gate host-side completo. Não significa que o agente
jogou o port nem substitui a instalação virgem e a aceitação humana do ZIP
exato, que continuam em milestones posteriores.

## Testes

Os testes usam somente fixtures locais e não acessam rede. Eles constroem ELFs
reais de teste e portanto requerem `aarch64-linux-gnu-gcc`, `clang` e `ld.lld`
além dos requisitos normais do gate:

```sh
bash framework/nxrelease/tests/test_nxrelease.sh
```
