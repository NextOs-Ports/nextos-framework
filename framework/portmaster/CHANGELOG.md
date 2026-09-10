# Changelog do contrato PortMaster

Versão do componente: **2.1.1** (contrato v3). O arquivo `VERSION` existe para
que o pin do framework possa fotografar este diretório junto dos demais
componentes; sem ele, um port construído a partir de um snapshot não consegue
rodar o gate de ciclo real do HarbourMaster.

## Runtime gerenciado — 2.1.1 (2026-08-30)

- O autenticador de generation-v2 reconhece o papel aditivo `runtime-data`
  somente em modo `0644`, preservando ordem, path e SHA-256.
- O ciclo real inclui um assembly gerenciado na geração e continua recusando
  papel desconhecido, modo executável, ordem ou bytes divergentes.
- Nenhuma regra do parser oficial, instalação, controles ou runtime vazio da
  2.1.0 foi relaxada.

## Contrato v3 — 2026-08-30

- fixa a tag oficial legada `2024.03.10-0841` e os hashes exatos do parser,
  instalador e formatador de runtime observados no dArkOS;
- corrige a representação de uma lista de runtimes vazia: em `port.json` v4 o
  campo `attr.runtime` deve ser omitido, embora a declaração do projeto continue
  obrigatória e validada como array;
- prova que a omissão completa o ciclo real no HarbourMaster atual, que a
  normaliza para `[]`, e reproduz o comportamento legado, que a normaliza para
  `None` e não entra no instalador de runtime;
- preserva o negativo histórico de metadado v1 sem runtime e não amplia qualquer
  alegação de compatibilidade legada para uma lista não vazia.

### Esclarecimento executável — 2026-08-28

- o negativo `update_stale_members=fatal` continua recusando qualquer arquivo
  aposentado comum;
- uma geração anterior completa e autenticada não é stale: o overlay preserva
  a v2 como rollback A/B; a v1 histórica continua control-only e inerte;
- o ciclo real agora distingue as duas classes pelo `GENERATION.json`, id,
  manifesto, commit e closure exata de path/modo/SHA-256, com negativos para
  arquivo apenas parecido com generation store, ordem/modo adulterado e colisão
  divergente do mesmo id.
- primeira adoção por ZIP sem store continua válida; recibos até nxgenerator
  0.2.18 preservam a ordem histórica de componentes de path, 0.2.19+ usa ordem
  POSIX textual, e o `execution_roles` opcional do v1 mixed-ABI é conferido
  contra o `nxport.json` armazenado.
- receipt sem store ou store sem receipt falha também em instalação limpa e no
  ZIP anterior; estado split jamais recebe a exceção de primeira adoção.

## Contrato v2 — 2026-08-16

- adiciona um schema estrito para o subconjunto suportado de `port.json` v4;
- torna `attr.runtime` obrigatório e cobre a falha real do parser quando o campo
  está ausente;
- fixa o HarbourMaster oficial no commit
  `8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967`, com manifesto SHA-256 e licenças;
- executa offline, em raiz temporária, os ciclos de instalação, descoberta,
  atualização, desinstalação e reinstalação sem iniciar launchers ou ELFs;
- rejeita JSON duplicado/truncado, tipos inválidos, traversal, divergência de
  caixa, itens ausentes e resíduos de uma versão anterior;
- integra o ciclo real ao auditor genérico de ZIP e ao runner seguro do framework.

O contrato v1 permanece válido e imutável. Esta versão é uma extensão aditiva e
não declara suporte físico a firmware ou aparelho.
