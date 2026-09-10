# Revisão antes de publicar

Este é o primeiro rascunho **privado**, preparado para revisão rápida de conteúdo e guias. O mantenedor precisa aprovar explicitamente a mudança de visibilidade.

Já incluído: exportação V5 com identidade por arquivo, fontes selecionadas de 41 repositórios/45 títulos, catálogo e orientação para uso com IA, guia de compilação ARM e exemplo de shims com teste host.

Ainda aberto:

- Revisar os guias e completar tradução inglesa e projeto Android/NDK demonstrativo.
- Consolidar exemplos completos de vídeo/áudio/input e extração NXExtract; o exemplo atual prova somente os contratos descritos.
- Fornecer uma rota pública verificada para toolchains/sysroots de baixa glibc e testar o guia em ambiente independente.
- Revisar omissões de fontes/builds/notices e licenças por arquivo, especialmente Goblin Sword, PartyBoard, Pikmin, Forager e FP2.
- Escolher a versão aprovada de cada referência. O HEAD público observado foi usado como snapshot inicial; não foi reclassificado como última versão fisicamente aprovada.
- Resolver a proposta não comercial para textos novos sem substituir direitos GPL/MIT do código existente.
- Converter a meta de cobertura dos shims em contratos e resultados medidos por família de engine.
- Definir a CI pública independente; doze testes históricos privados da V5 foram omitidos, e a suíte completa antiga depende de outros recursos do monorepositório.

Não é necessário reconstruir ou testar novamente os 45 ports para revisar uma coleção de fontes. Mudanças de executável, pin ou suporte exigem a validação específica correspondente.

`verify.py` verifica a integridade desta seleção e arquivos proibidos reconhecíveis. Não certifica titularidade de todo código, jogo completo, compilação de cada referência ou ausência de qualquer dado que pudesse estar codificado em texto.
