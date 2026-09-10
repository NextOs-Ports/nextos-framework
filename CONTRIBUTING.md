# Contribuir com NextOS

[English](CONTRIBUTING.en.md)

Leia [AGENTS.md](AGENTS.md), escolha o contrato que pretende melhorar e trabalhe em diretório/branch próprio. O repositório permanece privado até aprovação de NextOS.

## Documentação bilíngue

Atualize português e inglês na mesma mudança, incluindo links, exemplos, comandos, limitações e números. Uma página inglesa curta não substitui a tradução do guia completo. Registre o par em [publication/languages.json](publication/languages.json).

Mantenha identificadores de APIs, caminhos, enums e schemas iguais nos dois idiomas. Traduza explicações e prompts. Use links relativos existentes e declare placeholders de comandos antes de apresentá-los. Diferencie comando testado, sintaxe conferida e receita condicionada a SDK/input ainda não disponível.

Não traduza em cima das árvores históricas pinadas. Acrescente explicações externas; preserve hashes, arquivos de licença e notices upstream. Os textos legais normativos permanecem originais; a explicação de licença é bilíngue e não muda seus termos.

## Catálogo e Unity

O catálogo contém ports criados/integrados por NextOS. Para um repositório público, registre origem, commit e seleção de fontes com hashes. Para distribuição comunitária sem fonte pública, registre isso explicitamente e não invente URL ou snapshot. Cada título precisa de autorização de inclusão e escopo de evidência.

`portando_unity` só recebe os casos admitidos e ferramentas genéricas revisadas. Não copiar índices, galerias ou guias locais que incluam outros jogos. Um reparo parcial conserva a limitação; não herda gameplay de outra build do mesmo título.

## Código e dados

V5 e fontes de referência são preservadas. Mudanças compartilhadas de comportamento seguem V6 separadamente; um adapter novo não altera os ports vizinhos. Mantenha dados proprietários em área privada, fora de commits, issues e artefatos de CI. Não reproduza nome pessoal, endereço de aparelho, credencial ou origem de download de APK.

Use testes dirigidos para código novo. Para mudanças documentais, confira links, paridade de comandos, linguagem e integridade das referências, sem reconstruir jogos aprovados. Execute:

```sh
python3 publication/verify.py
python3 publication/verify-docs.py
git diff --check
```

## Commit e revisão

Créditos da coleção: **NextOS**, com notices obrigatórios de terceiros. Nunca adicionar assinatura/coautoria de IA a commits. Revise arquivos e mensagem antes do push. O PR/commit deve explicar o resultado, por que mudou, como foi verificado e limites materiais.

Publicar ZIP de jogo exige seus próprios [gates de entrega](docs/pt-BR/TESTES-E-ENTREGA.md). Corrigir um guia não autoriza mudar visibilidade, migrar pins ou criar uma release de jogo.
