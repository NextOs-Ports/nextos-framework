# Pins, procedência e recuperação de fontes

[English](../en/PINS-AND-SOURCES.md)

Uma tag histórica identifica a origem. Este repositório novo possui seu próprio histórico e não contém o objeto Git do commit V5 original. O procedimento abaixo preserva as duas identidades sem recriar a tag.

## 1. Materializar a seleção congelada

Use Python 3.11+ e Git 2.29+ ou o SDK público. Clone o histórico completo e execute da raiz:

```sh
python3 tools/pin_collection.py --destination work/my-collection-pin
```

O helper fixa `c5a5c827ed1918572a7cc01fc5a1740341b7a92d`, commit já publicado nesta coleção. Não usa `HEAD` móvel. O resultado é uma composição de 15 componentes, com commit/SHA e recibo. A origem continua V5 `657fb65a23b5c3b20040e76307b27e6470b1d17c`.

## 2. Entender as duas árvores

`source/` obedece exatamente ao contrato do `framework_pin.py` V5. Compilação do nxloader usa essa árvore. `tool-source/` contém a exportação completa selecionada, conferida arquivo a arquivo contra o manifesto fixado. O gerador usa esta segunda árvore porque precisa também de `framework/contracts/apkcompat`, diretório que o registro fechado do pin V5 não admite.

Não acrescente diretórios dentro de `source/`: isso invalida sua verificação. `TOOL-SOURCE.json` registra os arquivos da segunda árvore; nenhum componente canônico foi modificado. Os testes privados omitidos continuam omitidos. Essa composição não promete a suíte histórica completa nem muda o baseline dos ports existentes.

## 3. Recuperar uma fonte pública omitida

```sh
python3 tools/recover_reference.py fp2-nextos --path build_universal.sh   --destination work/recovered-fp2
```

O arquivo é obtido no commit do catálogo, com hash registrado, somente em `work/`. Quando já consta da seleção, o download deve corresponder ao SHA-256 do manifesto. Caracteres especiais do caminho são preservados na URL; arquivo e registro de procedência precisam ser novos e usam modo `0600`. O comando não executa o script nem altera `ports/*/upstream/`. Examine paths, dependências e licença antes de usar. Uma cópia pública pode conter caminhos históricos que não devem entrar em uma documentação nova.

O FP2 possui esse script no repositório público, embora ele tenha sido omitido da seleção. Já o build Chrono exige imagem local e uma fonte TTF não incluída; o SDK novo não torna seu script antigo automaticamente portável. Consulte o [catálogo por perfil](../../catalog/profiles.json) e prepare uma receita própria com dependências explícitas.

## 4. Verificar o alcance

Não remova hashes para aceitar outra biblioteca. Não confunda o pin da coleção, o pin de um componente, o commit do port e a identidade do input do dono. Um build novo tem sua própria origem e precisa de testes próprios. O [laboratório de shaders](../../examples/shader-lab/README.md) demonstra essa separação para as dependências do FP2.
