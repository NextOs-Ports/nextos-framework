# Framework NextOS v3 — Changelog

> Framework V3 fechado para promoção. A RC7 continua sendo a identidade dos
> gates host; o NXRelease 0.2.43 consome nxbootstrap 0.6.37 e nxgenerator
> 0.2.20. Os fechamentos `public-final` de BB1, BB2 e OTR permanecem históricos,
> sem ampliar os claims físicos além dos receipts exatos de cada artefato.

## Identidade da candidata auditada

- Integração RC7: commit
  `cad8a5b28a474a2feb63d1657f189e262bfa019b`, tags locais imutáveis
  `framework-v3-rc7-20260828` e `nxrelease-v0.2.41`.
- Gerador integrado: tag local imutável `nxgenerator-v0.2.19`, commit
  `7f5d33319ead748e333bbc11d763d4198b6c1a33`.
- Gates host completos da RC7: `ALL PASS 103/103`; auditoria PortMaster focada:
  `ALL PASS 29/29`. A RC7 está fechada no host sem ampliar alegações físicas.
- A promoção foi concluída com três candidatos imutáveis: BB1 1.0.2, BB2 1.0.4
  e OTR 1.0.3. Cada port passou pelo `public-final` do NXRelease 0.2.42 com os
  bytes físicos testados iguais às reproduções limpas A e B.

### Estado final após a RC7

- As tags `framework-v3-rc7-20260828` e `nxrelease-v0.2.41` permanecem como a
  identidade histórica imutável da RC7 em `cad8a5b`.
- A integração corrente avança nxbootstrap para `0.6.37`, nxgenerator para
  `0.2.20` e NXRelease para `0.2.43`. O
  `public-final` pode reproduzir candidatos físicos autenticados
  cuja metadata declare `0.2.40`, `0.2.41` ou `0.2.42`, preservando essa
  identidade em
  `NXRELEASE-METADATA.json` e `SBOM.cdx.json`; tested/build-A/build-B ainda
  precisam ser iguais byte a byte.
- O gate e `BUILD-PROVENANCE.json` identificam o executor corrente como
  `0.2.43`, e builds ordinárias emitem `0.2.43`. Essa
  compatibilidade fechada preserva NXExtract, NXSplash, interface visual e o
  fluxo nativo; somente novos candidatos opt-in recebem o launcher 0.6.37.

| Componente | Composição final V3 |
|---|---:|
| nxbootstrap | 0.6.37 |
| NXExtract | 1.2.21 |
| nxgenerator | 0.2.20 |
| nxrelease | 0.2.43 |
| nxgl | 0.2.17 |
| nxinput | 0.5.1 |
| nxcompat | 0.3.0 |
| nxaudio | 0.3.1 |
| nxandroid | 0.3.0 |
| nxobs | 0.3.1 |
| nxloader | 0.7.2 |
| nxabi | 0.2.0 |
| nxdoctor | 0.1.0 |
| nxsplash | 0.1.2 |

## Principais novidades

- **NX GPTK (`NEXTOSCONTROLLERS.gptk`)**: novo sistema próprio do NextOS para remapear controles por ação do jogo, com perfis separados para menu, gameplay, cursor e câmera. Não transforma o controle em teclado e preserva a configuração do usuário nas atualizações.
- **NEXTOSSETTINGS**: novo arquivo simples `NEXTOSSETTINGS.txt` para opções do port. A primeira versão oferece seleção de idioma e perfis de qualidade `auto`, `low`, `medium` e `high` quando suportados pelo jogo.
- **Idioma inteligente**: escolha por configuração do usuário, firmware ou locale do sistema, com fallback seguro e suporte aos idiomas realmente declarados por cada port.
- **Controles mais completos**: suporte a múltiplos destinos de input, touch com rotação e safe area, troca A/B respeitando o mapeamento do firmware e movimento de cursor/câmera estável em diferentes FPS.
- **Atualizações seguras por geração**: cada versão do runtime fica isolada e verificada. Uma atualização só é ativada após provar que iniciou corretamente; em caso de falha, o framework volta automaticamente à última geração saudável.
- **Auto-reparo e proteção de dados**: componentes do framework podem ser restaurados sem sobrescrever saves, APKs, OBBs, `gamedata`, controles ou configurações pessoais.
- **NXDoctor**: nova ferramenta de diagnóstico somente-leitura para verificar instalação, gerações, espaço, locks, logs, controles e settings, com ações explícitas de recuperação segura.
- **NXExtract aprimorado**: valida o conteúdo real do jogo em vez de prender a instalação a um único SHA, nome, assinatura ou empacotamento do APK. Passa a tratar melhor APKs base, splits, variantes e perfis de patch compatíveis.
- **Vídeo mais confiável**: validação do contexto EGL/GLES realmente obtido, drawable, shaders e driver usado antes de liberar o jogo. Inclui suporte a SDL3 empacotado, perfis de qualidade e correções opt-in para texturas single-channel.
- **Áudio monitorado de verdade**: confirmação do formato aberto, atividade dos callbacks, underruns e encerramento seguro do backend de áudio.
- **Melhor suporte multi-device e multi-ABI**: auditoria separada de jogo, loader, extrator, splash e helpers, incluindo combinações AArch64/ARMHF e dependências empacotadas por port.
- **Logs e suporte melhores**: eventos estruturados do launcher ao jogo, códigos de erro por categoria, recibos de vídeo/áudio e bundles de suporte sanitizados, sem dados pessoais.
- **Builds e releases mais seguras**: geração reproduzível, manifesto completo, hashes, validação de todos os arquivos do ZIP e exigência de evidência por aparelho antes de declarar compatibilidade.
- **Promoção explícita de adapters**: o gerador continua produzindo scaffold por padrão; um port só sai desse estado quando fornece contrato real, lifecycle e claims coerentes por um bloco de promoção validado. Nenhum port antigo é promovido automaticamente.
- **Pasta `gamedata` padronizada**: caminho claro para os dados fornecidos pelo dono, com instruções bilíngues e proteção contra substituição acidental.
- **Identidade visual preservada**: a interface gráfica do NXExtract e a NXSplash canônica de cinco segundos continuam com o visual aprovado, sem redesign no v3.

## Estado atual

- RC7 fechada no host em `cad8a5b`: bateria completa `103/103` e auditoria
  PortMaster focada `29/29` aprovadas.
- NXRelease 0.2.42 fechado funcionalmente em `a63bd06` e comprovado pelos
  `public-final` de BB1, BB2 e OTR. Em todos eles o ZIP testado e as duas
  reproduções limpas foram byte a byte idênticos.
- A composição final 0.6.37/0.2.20/0.2.43 preserva esses artefatos e não
  cria nova alegação física sem um candidato opt-in separado.
- A tag agregada final `framework-v3` congela essa composição promovida. Ela não
  transforma a prova de um port ou firmware em suporte universal para outro.
- A linha V3 fica congelada nessa tag; mudanças posteriores do framework começam
  na V4 e chegam aos ports somente por opt-in explícito.
- Validações físicas e pilotos continuam sendo feitos por port e por família de aparelho.
- Ports antigos não serão migrados automaticamente; a adoção do v3 será gradual e por versão nova de cada port.

## Escopo fechado para a release V3

- `V3-DISPLAY-01` foi adiado formalmente para V3.1. A V3 não publica uma
  política canônica de aspect ratio, viewport ou `content_rect`, não muda o
  comportamento visual existente e não implica stretch automático.
- O contrato de áudio, os perfis de qualidade, a instrumentação de performance,
  o preflight de storage/ENOSPC e o NXDoctor são publicados apenas no nível
  provado pelos gates host/sintéticos. Uma execução física de um piloto prova
  somente aquele ZIP, firmware e aparelho; ela não cria suporte universal.
- O adapter de texturas single-channel permanece opt-in e desligado por padrão.
  Seu claim físico depende do mesmo artefato final ser validado nas duas classes
  de GPU declaradas.
- O catálogo de CFW promove somente claims com receipt sanitizado ligado ao
  commit/tag e aos hashes exatos. Claims sem esse receipt mantêm o nível e a
  lacuna explícitos.
- A fronteira de promoção foi satisfeita sem herança de claims: os receipts de
  BB1, BB2 e OTR ligam GPTK A/B, ausência de duplo input e hashes aos bytes
  exatos de cada ZIP. O rollback físico `NXU0004` permanece provado somente no
  escopo Merchant/RC2 já versionado; a composição RC7 atual conserva a prova
  host de rollback e não declara um novo claim físico para esse cenário.
- As tags `nxextract-v1.2.21`, `nxgl-v0.2.17`, `nxinput-v0.5.1`,
  `nxcompat-v0.3.0`, `nxaudio-v0.3.1`, `nxandroid-v0.3.0`,
  `nxobs-v0.3.1`, `nxabi-v0.2.0`, `nxdoctor-v0.1.0` e
  `apkcompat-v1.1.0` congelam as versões integradas junto da tag agregada.
