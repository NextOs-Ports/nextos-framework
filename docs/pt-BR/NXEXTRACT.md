# NXExtract: dados do dono e instalação limpa

[English](../en/NXEXTRACT.md)

A [aula integrada](../../examples/first-port/README.md) fornece input autoral, hook transacional, instalação limpa, reempacotamento positivo e rollback real. Esses testes usam UI desativada no host; a UI gráfica canônica continua obrigatória para aceitar um port no aparelho.

Um port BYO-data distribui o código/runtime permitido, framework completo, receita e NXExtract com a UI gráfica canônica. Os dados vêm da cópia compatível fornecida pelo dono. Isso vale também para ZIPs de teste em repositórios privados.

## 1. Definir a identidade dos inputs

Registre nome/versão do jogo, package ID, ABIs, tamanho e SHA-256 do APK de referência testado. Identifique bibliotecas, metadata, bundles e arquivos companheiros críticos. Um input split precisa estar completo antes do teste; um APK de fixture sem payloads necessários não prova extração.

O SHA integral identifica a cópia testada. A compatibilidade da receita também deve validar package ID, contrato de versão, ABI, estrutura e payloads críticos. Não rejeite somente porque a assinatura, ordem de membros ou nome externo do container mudou, quando o conteúdo compatível é o mesmo.

## 2. Escrever a receita do port

Leia o [exemplo mínimo canônico](../../suportando_outros_devices/extrator-universal/examples/recipe-minimal.json) e a classe `Recipe` em `nxextract.py`. O exemplo mínimo é estrutural; a identidade de um jogo real precisa de validação adicional.

| Campo/área | Decisão a documentar |
| --- | --- |
| `id`, `version`, `title` | Identidade estável da receita e revisão |
| `abi_order` | Somente ABIs realmente implementadas; AArch64 primeiro |
| `input.search_dirs` | `gamedata` primeiro, seguindo a política do gerador |
| `extract` | Caminhos internos, destinos e validação por payload |
| Hooks | Transformações necessárias, fontes, ferramentas e hashes |
| `validate` | Conferência final dos dados que o runtime consumirá |
| `commit` e marcador | Publicação transacional e identidade da instalação |

Não invente nomes de chaves a partir desta tabela: use o schema/validador e exemplos da versão pinada. Hooks devem ser executáveis definidos e revisados, sem shell arbitrário escondido no JSON. Conversões de shader/textura precisam preservar objetos não alterados e produzir saídas conferíveis.

## 3. Conferir a estrutura antes do aparelho

Depois de criar `work/ports/demo/extractor.json` com dados reais do seu port, confira os comandos disponíveis e valide a receita:

```sh
python3 framework/nxgenerator/nxgenerator.py --help
python3 framework/nxrelease/nxrelease.py --help
python3 suportando_outros_devices/extrator-universal/nxextract.py recipe-check \
  --recipe work/ports/demo/extractor.json
```

`recipe-check` verifica a receita; não executa uma instalação nem cria prova gráfica. Gere a árvore do novo port com `nxgenerator.py <nxproject.json> --source-root <raiz-do-port> --output <diretorio-novo>`, depois de completar manifesto, runtime e pins. A saída deve ser nova; não gerar por cima de um port aprovado.

## 4. Descrever a instalação nos dois idiomas

Todo ZIP de port inclui `<port-id>/INSTALLATION.md`, com português e inglês no mesmo arquivo. Informe onde instalar o launcher e a pasta do port, onde colocar a cópia do dono, qual versão/ABI é compatível, tamanho/SHA de referência, o que aparece na primeira abertura e onde consultar o erro.

Não publique o nome original do APK quando ele revela sua origem de download, nem sites, grupos ou distribuidores. Identifique o conteúdo pelos campos técnicos. Não inclua APK/IPA/OBB, bibliotecas originais, assets, saves ou dados convertidos no ZIP.

## 5. Testar do zero com o candidato final

Use o mesmo ZIP imutável que será entregue e uma instalação isolada sem dados preparados nem marcador antigo. Preserve instalações e saves existentes. Forneça o input completo, abra o launcher canônico e observe descoberta, UI gráfica, extração, hooks, validação, receipt e NXSplash antes do runtime.

Confira os hashes finais contra a referência aprovada. Registre ZIP/SHA, receita, input completo, versão da UI, renderer visível e resultado. Adoção de dados existentes, receipt antigo, PID vivo ou um arquivo `ui.ready` isolado não provam esse fluxo. A UI deve conservar layout/cores em 640×480 e 1280×720 nas provas aplicáveis.

Controles temporários da UI pertencem a uma sessão privada de runtime, não ao cartão FAT/exFAT do jogo. Não enfraqueça permissões para aceitar um handshake inseguro. A receita não deve reconstruir o executável aprovado.

## 6. Separar diagnóstico e aprovação

Registre erros de identidade, espaço/estrutura, hook, renderer e validação como falhas diferentes. Um receipt de sucesso deve pertencer àquela tentativa. Consulte [testes e entrega](TESTES-E-ENTREGA.md) para vincular a prova aos bytes finais. Esta documentação não apresenta uma nova instalação comercial como já testada.
