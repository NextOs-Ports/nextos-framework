# nxgenerator

Composição V5 corrente: nxgenerator 0.4.5 com nxbootstrap 0.8.4. O gerador não
reimplementa a resolução nem a sanitização estreita de `$LIB`: ele lê a versão
e os sources canônicos do bootstrap, fixa seus SHA-256 no receipt e transporta
o launcher resultante.

`nxgenerator` fecha a camada de geração do M19 sem inventar comportamento da engine.

Desde o 0.3.15, `controls.schema` também aceita 3 (NEXTOS_CONTROLLERS/3):
o default gerado carrega exatamente uma linha `FACE_LAYOUT = auto|modern|retro`
no preâmbulo (default `auto`), e o opt-in `controls.controller_profiles`
pode pinar o par completo de variantes `controllers-modern.nxb`/
`controllers-retro.nxb` (`face_layout_variants`) — sempre só autoridade 3,
jamais acima das fontes vivas de mapping. Goldens V1/V2 permanecem
byte-idênticos e nenhum port aprovado é regenerado por conta própria.

Desde o 0.3.14, a autoridade de receita NXExtract é AUTENTICADA antes de
executar: caminho canônico fixo, recusa de symlink em cada componente,
abertura no-follow validada por `fstat`, leitura única com teto, SHA-256
igual à identidade pinada independente, versão extraída por AST dos bytes
verificados e execução somente do snapshot autenticado sob nome lógico
estável — falha nunca deixa módulo parcial no cache e mensagens nunca
revelam caminho pessoal.
Ele recebe um manifesto de projeto, reutiliza o gerador canônico do `nxbootstrap` e
publica uma árvore nova de forma no-replace.

Versão atual: **0.3.13**.

A 0.3.13 delega a validação estrutural integral do `extractor.json` à
autoridade canônica de receita — a classe `Recipe` do NXExtract 1.3.0,
carregada sempre do engine do framework e nunca de código do candidato, com
gate duplo de versão (constante do engine + arquivo `VERSION`) e mensagens
sem caminho pessoal. O generator preserva apenas as políticas realmente suas
(APK-variant via apkcompat 1.1.0 e `gamedata` primeiro em
`input.search_dirs`), provadas aditivas pela matriz. O NXExtract 1.3.0 não
muda em nada.

A 0.3.12 distingue o contrato estático do opt-in vivo no próprio cabeçalho de
`NEXTOSCONTROLLERS.gptk`. Sem `controls.runtime_mapping`, o arquivo declara que
é apenas o mapa dos controles nativos e que edições não chegam à engine. Com
`controls.runtime_mapping = "nxinput-gptk"`, o cabeçalho editável anterior é
preservado byte a byte e continua sujeito à prova live do nxrelease. Não existe
opt-in falso nem necessidade de ligar nxinput em um port que usa SDL nativo.

A composição corrente usa nxbootstrap 0.7.8. O gerador lê
dinamicamente `framework/nxbootstrap/VERSION` e fixa os hashes dos dois sources,
e o launcher gerado só promove uma
geração quando o receipt do adapter coincide com uma fronteira `runtime EXIT`
persistida e o filho exato retorna status 0. O pin da fonte e a identidade da
geração mudam. Quando `promotion` substitui o skeleton, o contrato owner deve
preservar exatamente o mesmo `input_controller_profiles` declarado em
`controls.controller_profiles`; ausência ou SHA divergente falha antes de
publicar. Os demais schemas e opt-ins permanecem iguais à 0.3.11.

A 0.3.10 torna a terceira autoridade do runtime nxinput obrigatória: todo
projeto com `controls.runtime_mapping = "nxinput-gptk"` precisa fixar e
embarcar um `controls.controller_profiles` habilitado. O gerador recusa antes
de publicar qualquer saída se o bundle estiver ausente ou desabilitado. Ports
sem o opt-in não recebem campo nem comportamento de controle novo; a identidade
e os receipts registram corretamente a versão 0.3.10. O nome é fixo:
`controllers.nxb`, exatamente o caminho que o runtime NXC6 declara antes da
inicialização dos joysticks.

A 0.3.9 transforma a ativação do GPTK vivo em política gerada e fechada:
contexto começa não provado, evento segue nativo enquanto a integração não
estiver pronta, todas as ações precisam de sink antes da ativação e cada
entrega precisa de ACK do consumidor real. Um adapter promovido precisa conter
o mesmo `input.runtime_contract`; marker ou log de load isolado não bastam.

A 0.3.8 introduziu o opt-in `controls.runtime_mapping = "nxinput-gptk"`.

A 0.3.7 integra o `nxbootstrap 0.7.4` e leva canonicamente dois opt-ins
fechados do `nxport` até o launcher gerado:

- `sdl_provider: "system"` declara que SDL1/SDL2 pertencem ao
  firmware/PortMaster. O gerador não escolhe device, CFW ou backend e não
  transforma uma biblioteca privada em correção de input ou vídeo.
- `video_proof: "required"` torna pixels não pretos medidos na fronteira real
  de `present` condição para health. PID, áudio, contexto criado, exit zero e
  captura DRM/GBM preta não satisfazem esse contrato.

Os campos são estritamente opcionais e não possuem default serializado. Sem
declaração, eles não aparecem no `nxport.json` nem entram como `null`.
Comparando duas gerações feitas pela própria 0.3.7, acrescentar os opt-ins não
altera o contrato do adapter nem os payloads alheios; seus valores literais
entram no manifesto canônico e, portanto, numa identidade de geração própria.

A 0.3.6 é a composição integrada na V4 aberta: o runtime gerenciado da 0.3.5
e o exportador de catálogo da 0.3.3 juntos, sem mudança de comportamento em
nenhum dos dois.

A 0.3.5 integra o `nxbootstrap 0.7.2` e materializa o papel aditivo
`runtime-data` dentro da geração V2. Assemblies e metadados gerenciados ficam
presos aos mesmos path, modo `0644` e SHA-256 da árvore ativa, store imutável e
seed visível, sem serem reinterpretados como ELF ou dados do dono.

A 0.3.4 adota o contrato PortMaster v3 e deixa de serializar `attr.runtime`
quando a lista declarada no projeto está vazia. A declaração
`portmaster.runtime: []` continua obrigatória no `nxproject`; somente o metadado
v4 gerado omite o campo, forma aceita tanto pelo HarbourMaster atual quanto pelo
oficial legado `2024.03.10-0841`. Listas não vazias permanecem preservadas.

A 0.3.3 acrescenta um exportador opt-in para o JSON editorial usado pelo site
e pelo aplicativo NextOS. Esse contrato permanece separado de `nxport.json`
(identidade/runtime) e `port.json` (metadata PortMaster): nenhum campo do site
é injetado nesses arquivos e o comportamento/payload normal do port permanece.
A adoção explícita da versão 0.3.3 reseala corretamente versão, receipt e
identidade da geração; por isso não se alega igualdade dos bytes do receipt.

O exportador recebe a ficha no formato exato fornecido pelo consumidor atual
(`id`, `name`, `description`, `build`, `links`, `images`, `installation`),
valida o schema fechado e cruza automaticamente o título e o nome do ZIP com o
`nxproject`/`nxport` verdadeiro. A saída não recebe campos extras de schema ou
bindings que quebrariam o consumidor atual. URLs HTTPS, paths de imagens,
SemVer, tipo da build, privacidade, origem proibida de APK, JSON estrito e
publicação sem sobrescrita falham fechados.

A 0.3.2 acrescenta, na C4, o opt-in `controls.schema = 2` (NEXTOSCONTROLLERS v2): o
default passa a listar os 18 controles por seção, com `null` para o que o port
não usa e `native` para o passthrough declarado. Sem o opt-in a saída fica
byte a byte igual ao baseline anterior à C4.

A 0.3.2 acrescenta o opt-in `controls.controller_profiles` (bundle
NXCONTROLLER_PROFILES/1 content-addressed dentro do ZIP, com hash fixado).
Sem a declaração o campo **não existe** no contrato regenerado e a saída fica
byte a byte igual ao baseline anterior à C3 — igualdade literal contra um
golden real em `tests/fixtures/c3-baseline/`.

A 0.3.1 acrescenta o opt-in `graphics.evidence_boundary=post-first-present`
(V4-GRAPHICS-04): declarado, a prova gráfica final do port passa a ser exigida
somente depois do primeiro present real do guest (nxgl 0.3.1); ausente, nada
muda — nenhum byte de port existente é alterado e nenhum launcher ativa a
fronteira por autodetecção.

A 0.3.0 acrescenta ao schema 3 três opt-ins declarativos da linha V4, todos
**desligados por omissão** e todos normalizados explicitamente no
`adapter-contract.json`, para que um leitor nunca precise adivinhar se o port
não declarou nada ou se o gerador perdeu o campo:

- `display` (V4-DISPLAY-01): **ausência é `game`** — o framework não instala
  nada e todo port já aprovado continua byte-idêntico ao ser regerado.
  `preserve` faz letterbox, muda pixels e por isso é opt-in explícito, nunca
  default. As políticas são finitas e nada é decidido por device, CFW, GPU ou
  nome de jogo.
- `graphics.egl_binding` (V4-GRAPHICS-03): ausência é desligado. Ligado, exige
  o inventário exato de imports EGL do guest, ordenado, incluindo
  obrigatoriamente `eglGetCurrentContext`, que é o símbolo que prova a posse do
  contexto corrente.
- `controls.sdl3_portmaster` (V4-CONTROLLERS-02): ausência é desligado. Ligado,
  o port fixa o SHA-256 da SDL3 privada que embarca.

A 0.2.20 permanece descrita abaixo.

Versão anterior: **0.2.20**. Ela integra o `nxbootstrap 0.6.37` e deriva os novos
pins de fonte do template e do gerador canônicos, sem reutilizar a identidade
0.2.19. A ordenação estritamente lexicográfica pelo path lógico POSIX
introduzida na 0.2.19, os schemas e as interfaces visuais permanecem
inalterados.

A composição autoral introduzida na 0.2.18 acrescenta ao schema 3 uma fronteira
fechada (`package_payload`) e tuning opt-in de cursor/câmera no GPTK. Arquivos
do autor são lidos uma única vez, por descritor
no-follow, conferidos por path/modo/SHA-256/tipo e retidos até a materialização;
os bytes finais entram no inventário antes de `GENERATION.json`. Sem os campos
novos, o conteúdo funcional das gerações anteriores permanece inalterado.

A composição real de geração v2 com NXExtract introduzida na 0.2.17 segue
preservada e agora usa o `nxbootstrap 0.6.37`:
quando o bootstrap já materializou recipe, engine, runner, runtime-env e UI, o
nxgenerator valida que esses arquivos são regulares, têm bytes e modos
canônicos e os reutiliza sem tentar criar ou sobrescrever a árvore. Um membro
divergente falha antes da publicação. A integração introduzida na 0.2.16 segue
inalterada:
um `nxport` schema 3 com `generation_runtime` recebe do projeto um
`runtime_root` relativo à raiz de fontes, e o gerador entrega essa raiz ao
nxbootstrap para materializar a closure exata de geração v2. Executável,
bibliotecas privadas, hooks declarados e NXSplash ficam ligados por
papel/caminho/modo/SHA-256 tanto na raiz ativa quanto na geração imutável. A
ausência da raiz, um symlink, modo/hash divergente ou closure incompleta falha
antes da publicação.

Os schemas legados do `nxport` e do `nxproject` continuam aditivos: projetos
nxport schema 1/2 não aceitam nem precisam de `runtime_root` e preservam sua
geração de controle v1. Nenhum port existente é regenerado automaticamente.

Em `schema_version: 3`, o manifesto declara também `controls.actions` (ID
semântico, tipo e sinks reais do adapter) e `controls.contexts` (`menu`,
`gameplay` e, quando existir, `cursor`). O default
`NEXTOSCONTROLLERS.gptk` é derivado somente dessa declaração: um port novo
nunca recebe `player.primary` ou outra ação genérica que seu adapter não
consome. O mapping SDL/PortMaster completo continua soberano sobre os ordinais
físicos; o GPTK atua uma única vez acima dos botões lógicos. O bloco opcional
`controls.tuning` acrescenta somente as chaves declaradas a `[cursor]` e
`[camera]`, sem reemitir defaults implícitos.

O scaffold com NXExtract ativo fixa `nxbootstrap 0.6.37`, engine NXExtract
1.2.21 e UI gráfica 1.2.16, registrados separadamente no recibo. A instalação só
libera a leitura dos dados após
o renderer gráfico SDL ou framebuffer apresentar a tela validada; TTY é apenas
diagnóstico e não atesta visibilidade. O splash gráfico de cinco segundos
continua obrigatório em toda abertura. Projetos anteriores não são regenerados.

O recibo agora fixa o NXSplash 0.1.2, incluindo o artefato que recupera provider,
VT ativo e framebuffer sem mudar a identidade visual já aprovada. Essa adoção
vale somente para árvores novas ou migrações explícitas.

O pin não é uma aceitação física do ELF AArch64 atual, SHA-256
`d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97`.
Esse artefato continua `physical-unverified` até o mesmo ZIP do port Hitman GO
ser aceito nos dois aparelhos autorizados; nenhuma matriz sintética substitui
essa prova.

## Entrada e saída

O schema atual está em
[`schema/nxproject-v3.schema.json`](schema/nxproject-v3.schema.json). Ele exige
os contratos V3 de idioma e controles, permite a promoção explícita descrita
abaixo e exige `runtime_root` somente quando o `nxport` interno usa schema 3.
O schema [`v2`](schema/nxproject-v2.schema.json) mantém
`portmaster.runtime` como lista explícita, inclusive quando vazia; o schema
[`v1`](schema/nxproject-v1.schema.json) continua aceito por compatibilidade e
recebe a mesma omissão segura no resultado quando não declara runtime; nenhum
projeto antigo é regenerado
automaticamente.
Há exemplos separados para
[`AArch64`](examples/nxproject-aarch64.example.json) e
[`ARMv7/ARMHF`](examples/nxproject-armv7.example.json), além do exemplo
[`mixed ABI`](examples/nxproject-mixed-armv7.example.json) que mantém NXExtract
AArch64 e executa NXSplash/jogo ARMHF.

Em projetos com `nxport.execution_roles`, cada artefato é escolhido pelo papel
que o executará: `extractor` decide o ELF do NXExtract UI e `splash` decide o
NXSplash. A arquitetura global continua sendo a do jogo e do metadata
PortMaster. O recibo `GENERATION.json` guarda os papéis completos e os pins de
artefato conservam ABI e SHA-256 independentes. Sem opt-in, a seleção histórica
pela arquitetura global é preservada.

```sh
python3 framework/nxgenerator/nxgenerator.py \
  framework/nxgenerator/examples/nxproject-aarch64.example.json \
  --output /tmp/nxexample-project
```

### JSON de catálogo do site/aplicativo

O contrato editorial v1 está em
[`schema/nextos-port-catalog-v1.schema.json`](schema/nextos-port-catalog-v1.schema.json)
e um modelo preenchível em
[`examples/nextos-port-catalog.example.json`](examples/nextos-port-catalog.example.json).
Ele não substitui `INSTALLATION.md`, não declara suporte físico e não altera o
catálogo técnico de evidências em `framework/catalog/`.

O schema fecha a **forma estrutural** para consumidores JSON genéricos. As
regras cruzadas que dependem do `nxproject` — identidade, repositório, nome do
ZIP, privacidade, assets e `gamedata/` — pertencem ao `catalog_export.py`; ler
somente o schema não executa `x-nextos-semantic-validation`. O objeto Chrono
Trigger fornecido por Ronax é fixture de compatibilidade de formato
(`FORMAT_FIXTURE_ONLY=YES`), não prova que o port, a release, o site ou os
assets existam ou tenham sido testados.

Depois de preencher a ficha editorial do port, o build exporta o arquivo pronto
para o site/aplicativo com um único comando:

```sh
python3 -B framework/nxgenerator/catalog_export.py \
  caminho/nextos-port-catalog.json \
  --nxproject caminho/nxproject.json \
  --source-root /checkout/congelado \
  --output caminho/de/staging/port-id.json
```

O `id` editorial é o slug canônico `<nxport.id>-nextos` (`chrono-nextos`),
enquanto `nxport.id` continua sendo o nome real do pacote (`chrono`). Se o ID
técnico já termina em `-nextos`, ele não é duplicado. A ligação não é inferida
silenciosamente: `links.source` precisa ser exatamente o repositório público
`https://github.com/NextOs-Ports/<id>`, `links.latest_release` precisa ser
`<source>/releases/latest/download/<nxport.id>.zip`, e `name` precisa coincidir
com `nxport.title` desconsiderando somente caixa. Com NXExtract, o resumo de
instalação também precisa citar `<nxport.id>/gamedata/`.

`build.type` é o canal editorial finito usado pelo consumidor. O exemplo real
usa `beta` com a versão SemVer final `1.1.0`, portanto `alpha`/`beta`/`rc` não
exigem sufixo prerelease; `stable`, por outro lado, recusa SemVer prerelease.

As imagens são referências editoriais externas sob `port-json/<id>/`; elas não
são copiadas para `port.json.attr.image` nem confundidas com a capa interna do
ZIP. `placeholder: true` declara somente referências, sem alegar que os assets
já existem. Com `placeholder: false`, `--assets-root` é obrigatório e o
exportador aplica checks limitados de formato: chunks/CRC/stream zlib do PNG,
container/chunk/dimensões do WebP, markers/frame/SOS/EOI do JPEG e SVG por
allowlist, sem script, CSS ativo, animação ou referência externa. Isso reduz
falsos arquivos, mas deliberadamente não substitui um decoder completo de
pixels; o pipeline do site ainda deve decodificar a imagem antes do deploy.
Para cada asset conferido, o CLI imprime os campos
`ASSET_SHA256=<hash>` e `PATH=<path>`: é um receipt pontual que o chamador pode
guardar, mas esses hashes não entram no JSON de sete campos e não autenticam
mudanças posteriores no
checkout do site. O exportador escreve UTF-8 determinístico, modo `0644`,
newline final e nunca sobrescreve um arquivo existente. Campos futuros exigem
uma nova versão do contrato; v1 rejeita propriedades desconhecidas.

Esta etapa é totalmente offline: o exportador não acessa a rede, não consulta
GitHub, não verifica a existência da release e não implanta o site/aplicativo.
Também não usa aparelho. A adoção pelo catálogo global e a repetição dos gates
de composição com o overlay V4 (nxbootstrap 0.7.0/NXExtract 1.3.0) ficam para a
integração; o gate HarbourMaster com seed já existente na 0.3.2 deve ser
preservado nessa resolução. O registro em
`framework/tests/test-matrix-v1.json`/`run-safe-gates.sh` também é
`INTEGRATION_PENDING`; esta owner branch não altera nem executa a bateria global.

Um port standalone pode manter `nxproject.json`, `extractor.json` e `LICENSE`
na própria raiz. Nesse caso, `--source-root /caminho/do/checkout` resolve
somente esses inputs relativos; o caminho host nunca entra nos bytes gerados.
Raiz inexistente, symlink ou input que atravesse symlink falha fechado.

Para geração v2, `runtime_root` segue a mesma fronteira: é um diretório
relativo ao `--source-root` (`.` pode nomear a própria raiz). Cada membro
declarado em `nxport.generation_runtime` precisa existir abaixo dele como
arquivo regular com modo e SHA-256 exatos. O valor relativo permanece no
`nxproject.json` reproduzível; o caminho absoluto do host nunca é serializado.

## Composição autoral fechada

`package_payload` existe somente no nxproject schema 3 e contém no máximo 128
registros, em ordem lexicográfica e com path único por `casefold`. Cada registro
tem exatamente `path`, `mode`, `sha256` e `kind` (`payload` ou
`license-notice`). O mesmo path canônico relativo ao `--source-root` é usado no
destino abaixo de `<port-id>/`; componentes vazios, ocultos, `.`, `..` e barra
invertida são recusados.

Cada fonte precisa ser arquivo regular, não-symlink, com um único link, modo
exato `0644` ou `0755`, SHA-256 exato e até 4 MiB. O conjunto fica limitado a
16 MiB; `0755` só é permitido abaixo de `tools/`. ELF, `.so`, APK/APKM/APKS/
XAPK e archives comuns são proibidos por nome e magic. O gerador também bloqueia
launcher, executável, `required_files`, membros de `generation_runtime`, nomes
gerados e `cover.png` (reservado ao renderer como `portmaster-image`), além de
namespaces de runtime/estado como `adapter/`, `defaults/`,
`gamedata/`, `nxextract/`, `lib/`, `.nxruntime/`, `.nxrelease/`, `saves/` e
`userdata/`.

Nenhum payload sobrescreve um membro existente. A única exceção são
`README.md` e `INSTALLATION.md`, ambos obrigatórios no array quando
`documentation.status` é `authored` e ambos obrigatoriamente com
`kind: payload`; `license-notice` não identifica documentação principal. Em
`scaffold`, nenhum deles pode aparecer.
Primeiro o gerador constrói toda a documentação-base, depois aplica os bytes
autorais retidos e só então calcula o inventário e o recibo. Assim o hash final
nunca descreve o scaffold quando o pacote carrega documentação concluída.

## Tuning GPTK opt-in

`controls.tuning.cursor` aceita `speed` (0.05..8), `deadzone` (0..0.9),
`response_curve` (0.25..4), `acceleration` (0..4) e `smoothing_ms` (0..500).
Ele exige que alguma ação vetorial `cursor.*` esteja realmente ligada em
qualquer contexto; não obriga criar `controls.contexts.cursor`. Quando esse
contexto não existe, o gerador preserva `menu`/`gameplay` e acrescenta uma
seção `[cursor]` contendo somente tuning. Quando existe, `RIGHT_STICK` deve ser
vetorial, R3 deve ser botão e A/D-pad não podem ser roubados pelo cursor; o nome
semântico do botão R3 não precisa começar por `cursor.`.

`controls.tuning.camera` aceita `sensitivity_x/y` (0.05..8), `deadzone`
(0..0.9), `response_curve` (0.25..4), `invert_x/y` booleanos e `authority`
`nextos|native`; `[camera]` é sempre anexada como seção exclusiva de tuning.
Campos desconhecidos, boolean usado como número, valor não finito ou fora do
intervalo falham fechado. Números saem em decimal local sem expoente. Nos
limites 0.05/0.9, que não têm representação binária exata, o renderer escreve
uma forma decimal determinística do mesmo `float32` efetivo, do lado aceito
pelas constantes C do parser; o valor runtime é preservado e o arquivo passa
no parser canônico do nxinput.

A saída contém:

```text
NXExample AArch64.sh
nxexample-aarch64/
├── nxport.json
├── nxsplash-nextos
├── nxproject.json
├── extractor.json
├── nxextract/
│   ├── nxextract.py
│   ├── run-extractor.sh
│   ├── nxextract-runtime-env.sh
│   └── nxextract-ui
├── adapter/adapter-contract.json
├── port.json
├── gameinfo.xml
├── INSTALLATION.md
├── README.md
├── LICENSE
└── GENERATION.json
```

O gerador usa a versão exata de `nxbootstrap` declarada por
[`../nxbootstrap/VERSION`](../nxbootstrap/VERSION). Desde o 0.6.0, o produto tem um
único launcher visível e autocontido: o comportamento pré-main é materializado nele, sem
biblioteca de runtime, `nxdeployment.json` ou `run.sh` secundário. O launcher e o
`nxport.json` e o `nxsplash-nextos` formam o conjunto mínimo gerado; os hashes do
template, gerador, source/manifesto do nxsplash e helper da ABI ficam pinados em
`GENERATION.json`. No opt-in schema 3, esse conjunto inclui também a closure
`generation_runtime`, e `.nxruntime/generations/<generation-id>/` nasce no
formato v2 antes de o launcher visível ser publicado.

O gerador rejeita antes da publicação launcher com modo inseguro, pin stale, manifesto
divergente ou qualquer artefato aposentado (`nxbootstrap-*.sh`,
`nxbootstrap.sh`, `nxdeployment.json` ou `run.sh`). Quando NXExtract está
ativo, a receita é obrigatória e o conjunto
`nxextract.py`, `run-extractor.sh`, `nxextract-runtime-env.sh` e `nxextract-ui`
vêm integralmente do engine NXExtract 1.2.21 canônico. O recibo
`GENERATION.json` fixa separadamente a versão do engine e a versão visual 1.2.16,
os sources do gerador, template, NXSplash e NXExtract, incluindo manifesto,
source, arquitetura e SHA-256 do UI ELF selecionado. Em mixed ABI, as
arquiteturas dos papéis extractor e splash são a autoridade desses dois
artefatos; AArch64 e ARMv7 nunca compartilham o mesmo ELF. O inventário registra modo e SHA-256 do launcher,
manifesto, helpers e demais arquivos.

O `port.json` também deixou de ser um JSON tolerante produzido à margem do
framework. O manifesto v2 é sua fonte declarativa: `title`, `arch`,
`min_glibc`, runtime não vazio, launcher e diretório são serializados
canonicamente. Runtime vazio é omitido somente no `port.json` v4. O recibo fixa
o contrato PortMaster v3, os schemas suportados e os commits exatos dos
HarbourMasters atual e legado. Os exemplos gerados passam pelo parser upstream e pelos ciclos
offline de instalação, descoberta, desinstalação e reinstalação. O
`INSTALLATION.md` bilíngue nasce obrigatório, mas permanece marcado como
scaffold até receber a identidade técnica exata dos dados legais do dono.
O `gameinfo.xml` mínimo também nasce canônico: contém somente um `game`, com
`path` derivado de `nxport.launcher_name` e `name` derivado de `nxport.title`,
escapados como XML. O gerador não inventa descrição, imagem, desenvolvedor ou
outro metadado editorial e inclui os bytes 0644 no inventário da geração.

Para APKs copiados como container, o scaffold não aceita compatibilidade presa
a um único hash/CRC/tamanho exato do arquivo externo. A receita sempre declara
package ID e prova compatibilidade por SHA-256 interno forte ou por uma árvore
interna requerida com padrões, caminhos e limites estruturais; alternativamente
pode listar duas ou mais identidades explícitas do container. Bounds e magic do
container são opcionais porque um hook pode transformar esse arquivo depois da
seleção, como ocorre no ScourgeBringer. Assim recompressão, assinatura ou
empacotamento diferente não reprovam conteúdo interno comprovadamente
compatível, sem abrir aceitação para outro jogo ou payload incompatível.

## Prova de controles: `controls.proof.effects` (0.4.2)

O roteiro `ON_DEVICE_AUTOMATED_INPUT_PROOF` é gerado das actions/contextos que o port
declara; o port só fornece a navegação, o `quit_guard`, o `owner_remap` e, desde a
0.4.2, os **efeitos**: o contexto que a ENGINE publica depois de uma ação. Um
`delivery` é a palavra do adaptador (o sink foi chamado); a engine pode ignorar a
entrada — no FP2 1.1.4 um hook na função de polling matou todos os botões e a sessão
de menu passou só com receipts. Com `effects`, o gerador emite
`expect: context_change` logo após o `delivery` daquela ação e o oráculo
(`nx-device-input-proof.py`, nxinput 0.11.6) exige um receipt `kind=context`
diferente do último provado antes do estímulo.

```json
"proof": {
  "effects": {
    "fp2.pause": { "context": "menu", "source_regex": "stage:paused", "contexts": ["gameplay"] }
  }
}
```

Declarar só ações cujo efeito deixa a sessão onde a cobertura gerada espera (START
que pausa é pressionado de novo pelo roteiro). `contexts` restringe os contextos em
que o estímulo produz o efeito (no título/menu principal do FP2, START não pausa).
Um confirm que troca de tela não é auto-reversível e fica sem efeito declarado.

## Fonte imutável antes do build

`FRAMEWORK-BUILD-PIN.json` não pode ser somente documentação. Se um `build.sh` compila
`$REPO/framework` diretamente, trocar de branch ou atualizar um componente entre
o pin e a compilação produz bytes cuja proveniência é falsa. O helper
[`framework_pin.py`](framework_pin.py) fecha essa fronteira antes de chamar o
compilador:

```sh
# Uma única vez, ao escolher os releases dos componentes:
python3 framework/nxgenerator/framework_pin.py create \
  --repository . \
  --component nxloader=COMMIT_OU_TAG \
  --component nxgl=OUTRO_COMMIT_OU_TAG \
  --component nxcompat=COMMIT_OU_TAG \
  --component nxsplash=COMMIT_OU_TAG \
  --output ports/meujogo/FRAMEWORK-BUILD-PIN.json

# Em todo build, para um destino novo:
python3 framework/nxgenerator/framework_pin.py materialize \
  --repository . \
  --pin ports/meujogo/FRAMEWORK-BUILD-PIN.json \
  --destination /caminho/temporario/framework-source

python3 framework/nxgenerator/framework_pin.py verify \
  --pin ports/meujogo/FRAMEWORK-BUILD-PIN.json \
  --snapshot /caminho/temporario/framework-source
```

`create` pode receber uma tag ou ref apenas como entrada de seleção; o arquivo
resultante guarda exclusivamente o SHA-1 completo do commit. `materialize` nunca
copia o checkout, não aceita ref móvel no pin, ignora `git replace` e remove do
subprocesso todas as variáveis `GIT_*` herdadas que poderiam redirecionar o
repositório. O executável Git também é resolvido apenas no path de sistema
`/usr/local/bin:/usr/bin:/bin`, não no `PATH` herdado. Ele percorre
commit/árvores/blobs via `git cat-file`, recalcula o
SHA-1 de cada objeto, bloqueia lazy fetch e filtros, confere o `VERSION` contido
em cada commit, reconstitui uma árvore nova e publica com no-overwrite. Limites
explícitos de tempo, tamanho e quantidade mantêm o gate local e finito. O compilador deve receber
includes/sources somente de `<snapshot>/framework`, preferencialmente montado
read-only no container.

O repositório de origem precisa conter localmente todos os objetos: clones parciais,
configuração promisor e packs `.promisor` são rejeitados. O gate não consulta rede nem
executa helper para completar objeto ausente.

O schema está em
[`schema/framework-build-pin-v1.schema.json`](schema/framework-build-pin-v1.schema.json).
Seu digest `nxgenerator-component-tree-sha256-v1` é:

```text
SHA256(
  "nxgenerator-component-tree-sha256-v1\\0" ||
  para cada arquivo, ordenado pelos bytes UTF-8 do caminho relativo:
    git_mode_ascii_6 || "\\0" || path_utf8 || "\\0" ||
    decimal_size_ascii || "\\0" || hex_sha256_do_blob_ascii || "\\n"
)
```

Somente modos Git `100644` e `100755` são aceitos. Symlinks (`120000`),
gitlinks/submodules (`160000`), paths não canônicos, componentes fora do registry,
diretórios Git vazios que o snapshot não representaria e propriedades extras falham.
O snapshot usa diretórios `0755`, preserva arquivos
como `0644`/`0755`, normaliza todo mtime para `2000-01-01T00:00:00Z` e contém o
recibo canônico `FRAMEWORK-SOURCE.json`, ligado ao SHA-256 exato do pin.

O workspace e o diretório pai do snapshot devem pertencer ao mesmo usuário de
build e não ser graváveis por adversários durante a operação. O helper rejeita
ancestrais symlink e usa abertura no-follow, identidade de inode e publicação
atômica no-replace; depois da verificação, o snapshot deve ser montado read-only
no container para congelar a janela entre gate e compilador.

O contrato antigo `nextos-framework-release-pin-v1` não é aceito como build pin:
`source_archive_sha256` não definia uma serialização reprodutível. Releases
históricos permanecem históricos e cada port migra por opt-in. NXExtract também
permanece fora deste pin de árvore, pois é vendorizado e fixado separadamente em
`GENERATION.json`.

## Scaffold por padrão e promoção explícita

`adapter-contract.json` nasce como `unimplemented_nonrelease`, sem ordem de lifecycle,
JNI, callbacks, offsets, formato de áudio, mapping, save ou ação terminal. O gerador não
adivinha nenhuma dessas decisões. Elas só podem ser preenchidas pelo adapter do jogo com
fonte e teste próprios.

Um port que já implementou o adapter pode optar pelo bloco `promotion` do
`nxproject` v3. O bloco aponta para um `adapter-contract.json` real e declara
os três claims de promoção. O gerador exige contrato
`implemented_release`, lifecycle implementado e igualdade entre os contratos
de idioma, GPTK e gráficos do projeto e do adapter; depois materializa esses
bytes de forma determinística. Ausência do bloco conserva o scaffold.

Essa cópia não cria prova física. `physical_support_proven: true` só pode
chegar à release quando o `nxrelease` validar os receipts ligados à geração e
ao artefato exatos. Um port existente só adota a 0.2.20 por nova versão e novo
ZIP; nenhum port é regenerado automaticamente.

## Documentação pública

O README gerado segue a organização bilíngue do README aprovado do GTA San Andreas:
visão geral, arquitetura, problemas resolvidos, controles, dados, build/run, mapa de
fontes e licenças. Ele também separa baseline de suporte físico, explica quirks estreitos,
marca standalone como desenvolvimento e exige o mesmo ZIP/SHA para qualquer alegação.

O texto inicial é um esqueleto não publicável. Nenhum endereço, hostname, caminho
pessoal, credencial ou log bruto é aceito como evidência pública, e nenhuma linha gerada
declara suporte físico.

## Gate

```sh
python3 -B framework/nxgenerator/tests/test_nxgenerator.py
python3 -B framework/nxgenerator/tests/test_generation_runtime.py
python3 -B framework/nxgenerator/tests/test_package_payload.py
python3 -B framework/nxgenerator/tests/test_framework_pin.py
python3 -B framework/nxgenerator/tests/test_m19_closure.py
python3 -B framework/nxgenerator/tests/test_nextos_catalog.py
python3 -B framework/nxgenerator/tests/test_video_provider_contract.py
```

O primeiro gate gera duas árvores limpas para cada ABI e compara bytes e modos. Também
confere o launcher autocontido atual, rejeita dezenove adulterações/artefatos aposentados
e seis adulterações do release NXExtract (inclusive bytes ou paths cruzados entre ABIs),
valida pins, metadata PortMaster, NXExtract, documentação, contenção de paths e publicação
no-overwrite. Todos os caminhos shell públicos gerados são auditados contra dependência
do comando externo `stat`. A matriz inclui uma regressão que move a validação dos arquivos
antes do NXExtract e exige rejeição, mantendo a ordem canônica
`BIN → handoff → NXExtract → required files → nxsplash → adapter/libs → launch`.
Ele ainda rejeita quatro formas inválidas de `runtime`, prova a omissão segura
quando vazio, preserva listas não vazias, prova compatibilidade aditiva do
manifesto v1 e submete os dois ZIPs sintéticos gerados ao
HarbourMaster fixado, sem rede e sem executar código do port.

O gate focado de geração v2 produz um projeto real com nxproject/nxport schema
3, executável, biblioteca privada e hook vindos de `runtime_root`; confere as
duas cópias da closure, os recibos v2 e a identidade única. Também recusa raiz
ausente, diretório inexistente, hash stale e uso fora do schema 3, e prova
separadamente que um projeto schema 2 continua emitindo geração v1 sem exigir
essa entrada.
O gate de composição autoral prova substituição dos dois documentos, payloads
0644/0755, `kind`, inventário, determinismo e retenção anti-TOCTOU; os negativos
cobrem shape, ordem/casefold, limites, symlink/hardlink, modos/hash, archives,
namespaces e colisões. O mesmo gate renderiza os dois blocos de tuning, compila
o parser C canônico do nxinput e faz o arquivo gerado atravessá-lo, incluindo
os limites float32 e o cursor ligado em menu sem contexto dedicado.
O gate de framework pin
usa repositórios Git sintéticos e o tag real anterior para provar independência
de checkout/ref/locale/umask/path, publicação no-overwrite e rejeição de
adulterações. O gate de fechamento liga cada requisito M19 à implementação e ao
teste.
O gate de catálogo passa a estrutura enviada por Ronax pelo CLI completo,
exporta duas cópias byte-idênticas e cobre schema exato, binding entre catálogo
e nxport, SemVer/tipos, URLs/DNS, paths, quatro fixtures reais passando os
checks limitados de formato, privacidade, JSON com duplicata/BOM/NaN, modos,
symlink/hardlink, limite de tamanho e publicação no-overwrite. Os gates C3/C4
continuam provando que o core e os goldens de controles não foram alterados.
O gate de provider/vídeo gera uma árvore sem declaração e outra com os dois
opt-ins; prova omissão literal no primeiro caso, round-trip no segundo, pin do
`nxbootstrap 0.7.8`, ativação exata no launcher e bytes idênticos em todos os
artefatos alheios ao contrato. Valores abertos ou aproximados falham antes da
publicação. `test_generation_runtime.py` importa e executa esse gate como
pré-requisito, captura sua saída e mantém sua própria interface estável; assim
ele não pode ficar órfão quando a integração de geração for executada.
