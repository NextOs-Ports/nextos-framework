# Freedom Planet 2 — referência NextOS

[English](README.en.md)

[Catálogo](../../catalog/README.md) · [Guia para IA](../../docs/pt-BR/PORTAR-COM-IA.md) · [Fontes selecionadas](upstream/)

Origem: [fp2-nextos](https://github.com/NextOs-Ports/fp2-nextos), commit `f8caab9d946c39041fa5cc25cb52621590f82adc`. Plataforma: Android.

## Como usar esta referência

Esta pasta contém uma seleção de fontes públicas de um port criado/integrado por NextOS. Não é um jogo completo nem pacote instalável. Nenhum dado comercial acompanha esta seleção; o novo port precisa da cópia compatível fornecida pelo dono.

Leia [SOURCE-MAP.json](SOURCE-MAP.json) antes de copiar código: há 227 arquivos incluídos e 2 omissões adicionais registradas de privacidade/conteúdo. Receitas upstream podem exigir arquivos fora desta seleção; um clone desta pasta não garante build autônomo.

Compare engine/build, ABI, assinatura, dados, renderer, áudio e consumidor de input. Escreva o novo adapter em outro diretório. Registre a fonte/hash/licença da peça e teste seu contrato no destino.

## Status e limites

O snapshot é o commit público observado, não uma nova certificação física. Leia o status da versão exata antes de alegar gameplay, instalação ou suporte a aparelhos. Os próprios pins da referência foram preservados: sua presença junto da V5 não migra V3/V4/V6 nem recompila binários aprovados.

## Licença e autoria

O GitHub detecta MIT, mas NOTICE.md declara loader/hooks/GLES GPL-3.0-only, NXExtract MIT com módulo GPL e splash MIT. Não tratar o port inteiro como MIT.

Autoria da integração/coleção: **NextOS** — [GitHub oficial](https://github.com/NextOs-Ports). Preserve avisos de licença e créditos de terceiros. Dados, código original e marcas do jogo conservam seus respectivos titulares. [Termos da coleção](../../LICENSING.md).

Os arquivos dentro de `upstream/` preservam seus bytes e idiomas originais. Esta ficha e os guias externos oferecem a navegação bilíngue sem alterar a referência auditada.
