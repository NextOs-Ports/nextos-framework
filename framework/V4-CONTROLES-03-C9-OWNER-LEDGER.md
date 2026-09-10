# V4-CONTROLLERS-03 / C9 — ledger de owners e integração

Estado deste ledger: `INTEGRATED_OPEN`

Data: 2026-08-30

Base imutável da integração:

`6af79b6411c5115ffbd43be244a35ad167bd5a49`

Tree da base:

`7a0d16765debbe5f1245e958e3cc4a92430836e8`

V4 permanece aberta: `YES`

Este arquivo foi criado e versionado antes do primeiro merge da C9. Ele fixa
os heads terminais que podem entrar na integração, não os heads intermediários
de cada categoria histórica. Nenhum port ou piloto integra a árvore comum.

## Gate direto C1–C8

As baterias anteriores não foram repetidas. Relatórios, artefatos, commits,
trees, manifests, resultados e logs foram lidos novamente do disco e seus
hashes foram recomputados.

| Gate | Estado aceito | Marker SHA-256 | Relatório SHA-256 | Evidência terminal |
|---|---|---|---|---|
| C1 / 112 | PASS, corpus selado | `af2b175c57f9763a79abbd0f41469884f52e34619a96fdf46f9f4952f61668a4` | `e5272fe8b2f131128f3343215640423f65ec6ce796e0332e8ddc1c066df521c7` | manifesto 4/4; corpus `c5a2da52a428859dee97441aa174451d4554293ad159047991ecd42d6143cb68`, 946 artefatos, pin upstream `8d3a5db24bb0e8944b71242c6a00c56a08702fe4` |
| C2 / 113 | PASS, observed-only | `c405d34f134e82f14d78ceba9f48522a4c29257e1361e4df89148bb9fabe75ab` | `73de85a6a7f65d64629a817f89dc789f2bcd0084f3ebfcde75ebdb2f988deed2` | seis receipts presentes; nxinput `17791dae` ancestral do terminal e nxobs `6f4d3c7a` terminal |
| C3 / 114A | PASS, sucessor corrigido | `73d11712b72f76e35495c4c10eb8eb794e8e6a8742449243f388da51e9889998` | `1260de7406168a395987266887daefcb5bc69882d00f216f3ef7564202416b34` | log `dde4d2a4157a98074fcaa3b3862a7ab011a3d14bb9ccb50576f11152b2f79b19`; 10/10, corpus integral, mutante recusado, bytes sem opt-in preservados |
| C4 / 115 | PASS, core hermético | `908e3b692f1de72a448e2cf67f6dac36ef7dfd43f7ca2a2f81625932af44fc33` | `de4506278dc7fbcb4d3540c5e266b5f770b34939a45e6b082ab159631788014c` | log `c8dbc27479bbf1ec53c5d823f0f3237a9af5a912e25ff06cd588438d9248a56e`; 13/13, 18 controles, action/null/native, lifecycle fora de banda |
| C5B / 116B | PASS | `640aa0d869debce574e0e53e0b6409d26827ce76b1c2359e05c3f5332f2d4d0e` | `e3d0830ce62dc838490534e8115f3e39a6ea91492d29e0eacfb94453f6c9fd1e` | log 0444 `523e38364623aeceb2668617f59224d5039803ba452ee4f5b0cfd44677e75bf1`; 9 gates uma vez, 552/552, auditoria externa VALIDATED |
| C6 / 117 | PASS | `41aa1c3f13d999a695171a071bb2ed6effb7ae3fc2de7c62da4f32afefa1fd8c` | `fc46cca656b79ce2254c72714d979cfb4e4d3d821061b916136c31eb6ea2fc4d` | log 0444 `860db62d8e0401c00e6516502c89081335140341329da3533f0add41527abfae`; 5 gates uma vez, 1077/1077 |
| C7 / 118 | PASS | `044a2733ea0b87dfe34ef68034a47d6dad49d4083f708ee375f21492751b6b06` | `26baff6e779db11ceaacd8c3f130ca4471bb1b9efb0ed471b70a2b3f4f6b0c6d` | log 0444 `11d1d91999cfcd0f48590491e6fe459c0758d352d4bf1226d655ae938e0f6983`; 6 gates, 339/339 |
| C8 / 119 | PASS, owner reconciliado | `44531ad957b246146c3770e155c739b405feeec3d665f61ac67542aa5d346808` | `9e6dffa96c0f91baf29e77b11e940be8508d82bf5c25f7d83c88af90a5e6c456` | C8 original: log 0444 `4b5e250d88f34a0c33dfc131e237cb50c60751679fdc47b201d61d1a4e454f56`, 5 gates, 3130/3130; reconciliação: log 0444 `9d73a9b8b256a8b499dbd58ce5c5c2ef83c93a9e6f61de45f7210ad50dff2a4d` |

C1 não era uma bateria executável: sua autoridade é o corpus determinístico e
o manifesto 4/4. C2 antecede o formato de attempt imutável e sua autoridade é
o código terminal, o relatório e os receipts; não se inventa um log retroativo.
Os logs legados C3/C4 tinham modo original 0644; cópias byte-idênticas foram
seladas 0444 pela C9, sem alterar os originais. C5B–C8 já possuem diretórios,
logs, manifests e resultados exclusivos.

## Rejeições e sucessões preservadas

- C3 original (`b763c2d8`, `b0e2075e`, `7ac81900`) está explicitamente
  rejeitada. Os sucessores aceitos são `a7d397df`, `00d38753`, `2f12e7db`, e
  todos são ancestrais dos heads terminais abaixo.
- C5 original e 116A continuam rejeitadas. Somente C5B/116B é aceita. O marker
  C5B originalmente auditado tinha SHA-256
  `3771beb3a886d413e9a6463386ce332a4bca15effc5d14bf744fae856bd5d86b`;
  a C9 preservou esses bytes e apenas acrescentou as chaves literais
  `VERDICT=PASS` e `MISSION=116B_C5B` ao sucessor canônico.
- O marker C6 originalmente auditado tinha SHA-256
  `75c4c4302a3ae504e544520471b80f0af2cbb8226fbf2452235b55c8d0cfb55e`;
  a normalização canônica acrescenta `VERDICT=PASS` e `MISSION=117_C6`.
- C7/C8 tiveram apenas seus hashes encadeados atualizados. Os artefatos
  anteriores permanecem preservados; nenhuma bateria foi repetida.
- Tentativas interrompidas ou falhas permanecem históricas e não são usadas
  como evidência positiva.

## Heads terminais aprovados

`componente → branch owner → head terminal → tree total → subtree → gate`

| Componente | Branch owner | Head terminal aprovado | Tree total | Subtree `framework/<componente>` | Gate |
|---|---|---|---|---|---|
| nxinput | `framework/nxinput-0.7.0-v4` | `cc9db69be3d98a99f34a53e9d3413b0d7495083c` | `3e35c93e33c5b87ceb6cc2fea96d211de3550f7d` | `3e59ae088b6b10525742cc237a9a1a1c3e5f7b41` | C2–C6, terminal C6 |
| nxandroid | `framework/nxandroid-0.5.0-v4-c9-reconcile` | `182e3b8e08410158996af2c8b10901e6ea2c82a3` | `94de96e44a7ae9919821b4b7e754750e6b2a3457` | `098316c0f96417bb3bf9ec49d391a735c902f546` | C7/C8 + reconciliação dirigida C9 |
| nxobs | `framework/nxobs-0.4.2-v4` | `6f4d3c7a8e45f4ef55d4d7eaf96af661c0e98435` | `3d45bba1150444be7a866edd005c00298db85661` | `c25a616326eb1378b4d979405dcfbfaed27a402c` | C2 |
| nxgenerator | `framework/nxgenerator-0.3.2-v4` | `746a8a260656d6c3f034b5cf40a415ea16167f1a` | `962d0c3dfda1cae65a02b4cad2c01acf62b63597` | `1c8cdbaf4c9351dc995c617d0a88bb165d825b46` | C3 corrigida + C4 |
| nxrelease | `framework/nxrelease-0.3.2-v4` | `384eeb28272f3fa0c0ac24f572364f4045b2640b` | `1b9548b9971e1e15f45f895b87fa9b09dbf5b949` | `7d7b1073388828ec37e4d504feb0ddfad0e138c9` | C3 corrigida + C4 |

## Auditoria dos owners

| Componente | Merge-base com integração | Commits no range | Escape do owner | Worktree suja | Termos proibidos | Assinaturas |
|---|---:|---:|---:|---:|---:|---:|
| nxinput | `c9422f0b9984235387150f29638cff3fe1ac55dc` | 12 | 0 | 0 | 0 | 0 |
| nxandroid | `6af79b6411c5115ffbd43be244a35ad167bd5a49` | 5 | 0 | 0 | 0 | 0 |
| nxobs | `4de95252c65f80d214ebbb32550b1abe13f66b6b` | 1 | 0 | 0 | 0 | 0 |
| nxgenerator | `1c83799512f35b3af3e96eb084166d351873429b` | 3 | 0 | 0 | 0 | 0 |
| nxrelease | `1af88e9073ca5eb49d8a3f81503f9242af585003` | 3 | 0 | 0 | 0 | 0 |

Todos possuem VERSION, README, changelog, testes e seus contratos/matrizes
internos aplicáveis no owner. O diff efetivo do monorepo foi conferido e cada
range toca somente `framework/<componente>`.

## Reconciliação nxandroid

O head C8 original `8d0cf2b051866e097ea3ebd2d50fa49ef26e0e65`
partia de um baseline anterior ao subtree já presente na integração. Um merge
direto perderia o contrato M11/input-sinks e seis arquivos já aprovados. A
reconciliação foi feita no owner, preserva byte a byte o header/source Unity,
o ledger e o runner C8, e alinha o header público e os pins internos à versão
0.5.0. O único pin pré-merge deliberadamente futuro é o contrato agregado com
SHA-256
`1474a8b2e443da34b9cdfddf41ec9a3fc52c3389fc14dcacb75b0583b3599c7a`.
Ele só pode promover após os merges e a atualização documental agregada.

## Merges executados na ordem autorizada

1. nxinput;
2. nxandroid;
3. nxobs;
4. nxgenerator;
5. nxrelease.

| Ordem | Componente | Merge commit explícito `--no-ff` | Tree da integração após merge | Subtree igual ao owner |
|---:|---|---|---|---|
| 1 | nxinput | `c74a1809e7eb0579005b398a3e6ed18627a348c3` | `0847ddd8edb0eda162c587b82dc640ce11ab6d8f` | `3e59ae088b6b10525742cc237a9a1a1c3e5f7b41` = owner |
| 2 | nxandroid | `4bd0cbafc9ee1ddaefc418926af8d9f30ac84e7e` | `11cffbc3c3eee6e6b07bd9495653d24e45a4fa4a` | `098316c0f96417bb3bf9ec49d391a735c902f546` = owner |
| 3 | nxobs | `f7e62e543bf5f72a466e9faa75f22b205ab8508f` | `291b0ab81dac4afeac29d61b3ce5e38c6a1ab3ed` | `c25a616326eb1378b4d979405dcfbfaed27a402c` = owner |
| 4 | nxgenerator | `18270551d1b82adc950c4373a30c00478bd5fd50` | `44fb74fec1581a1c4d0795fefebef2c3d1e693cc` | `1c8cdbaf4c9351dc995c617d0a88bb165d825b46` = owner |
| 5 | nxrelease | `59836ae8dac925f29f7e5766967edbaaca59303b` | `28eaead446bc98d4de7e1179dc170828e87a4dfa` | `7d7b1073388828ec37e4d504feb0ddfad0e138c9` = owner |

Nenhum merge teve conflito e nenhum byte interno foi resolvido exclusivamente
na integração. Cada comparação foi feita imediatamente após seu merge. O HEAD
final volta a comparar as cinco subtrees com os mesmos hashes.

## Registros agregados pós-merge

- contrato declarativo agregado:
  `framework/contracts/declarative-v1.json`, SHA-256
  `1474a8b2e443da34b9cdfddf41ec9a3fc52c3389fc14dcacb75b0583b3599c7a`;
- changelog agregado: `framework/CHANGELOG-V4.md`;
- matriz esparsa: `framework/V4-CONTROLES-03-C9-SPARSE-MATRIX.md`;
- este ledger;
- runner externo one-shot `run-v4-controls-c9-final.sh`, SHA-256
  `77667c7e1c669ba3ae7540f98c9c2397f37be182d6573fa297a952df4261bd55`.

O contrato agregado registra nxinput 0.7.0, nxandroid 0.5.0, nxobs 0.4.2,
nxgenerator 0.3.2 e nxrelease 0.3.2, todos aditivos/default-off. Seu hash já
estava fixado no owner nxandroid antes dos merges; o gate M11 integrado o
recomputou e passou. O commit que contém esta revisão é o HEAD agregado
congelado; seu hash literal e sua tree são registrados no relatório e no marker
C9, evitando uma referência circular dentro do próprio commit.

## Tentativa C9 rejeitada e preservada

`C9-attempt-fe4e8c221a06-20260830T092436Z` executou uma vez no head
`fe4e8c221a060de562aed13613ee0b47e2355aa6`: 9/10 gates passaram. O gate final
rejeitou os espaços/tabs literais guardados dentro dos cinco payloads `.patch`
Godot/SDL, porque `git diff --check` interpretou as linhas de patch como código
ordinário. Nenhum gate de produto falhou.

O log 0444 foi preservado com SHA-256
`72a381fbddfc218256e13d9243150125b5cbe2b29e1a2d7abfd368e6bdf2cafc` e nunca
será completado ou reutilizado. O sucessor do runner continua auditando todo o
range, mas exclui dessa verificação genérica somente
`framework/nxinput/engine-patches/*.patch`; os bytes desses payloads continuam
autenticados diretamente pelas provas C5B/C6. Uma nova tentativa só pode rodar
no commit sucessor que contém esta correção documental.

## Limites

- `PHYSICAL=PENDING_PHYSICAL` para os novos heads V4;
- C1 contém uma linha física histórica vinculada ao artefato/stack histórico,
  nunca promovida ao HEAD integrado;
- Godot e SDL são `REAL_API_HOST`; Android/Unity novos permanecem
  `FIXTURE`/`PENDING`/`UNPROVEN` conforme seus relatórios;
- sem contato com device ou rede na C9;
- sem ZIP, tag, release, push, branch principal, V3 ou migração de ports;
- V4 permanece aberta.
