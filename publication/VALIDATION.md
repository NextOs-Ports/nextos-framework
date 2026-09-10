# Validação da edição bilíngue

[English](VALIDATION.en.md)

Data: 10/09/2026. Escopo: documentação, catálogo e importação selecionada Unity.

- Coleção original: 7.715 hashes de fontes/auxiliares, 41 repositórios/45 títulos e oito ELFs auxiliares V5 pinados.
- Catálogo ampliado: 47 títulos NextOS, incluindo duas fichas comunitárias sem snapshot de código.
- Exemplo de shims: build C99 host e CTest 1/1 PASS da preparação anterior; fontes e binário preservados, sem rebuild documental.
- Integridade atual: 7.725 hashes conferidos, incluindo dez arquivos genéricos/fixtures Unity; nenhum runtime V5 ou fonte upstream foi alterado.
- Documentação: 86 pares PT/EN, 1.344 links locais e 32 blocos shell com sintaxe válida; exemplos executáveis iguais nos dois idiomas. Conferência estrutural não substitui revisão semântica da tradução.
- Ferramentas Unity: 15 testes do planejador de texturas e 20 do verificador de toque passaram; exemplos CLI sintéticos executados com status 0 e sem alegação de prova física.
- Interfaces CLI de nxgenerator, nxrelease e NXExtract recipe-check conferidas por `--help`; nenhuma geração, instalação ou bateria de release foi iniciada.
- Privacidade/pacotes reconhecidos e `git diff --check`: PASS. Os 15 casos Unity têm vínculo ao commit e aos arquivos selecionados dos repositórios públicos, cuja visibilidade foi consultada nesta revisão.
- Não executado nesta revisão: build ARM/NDK/Godot, jogo comercial, extração no aparelho, teste físico, reconstrução dos ports ou suíte histórica completa V5.

Integridade de fontes, consistência de documentação e testes sintéticos não certificam gameplay nem licenças de cada arquivo. [Pendências](REVIEW.md).
