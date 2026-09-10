# Revisão antes de publicar

[English](REVIEW.en.md)

O mantenedor precisa aprovar explicitamente a mudança de visibilidade. Esta preparação continua privada.

## Incluído nesta edição

V5 preservada; fontes de 41 repositórios/45 títulos; duas fichas comunitárias adicionais; guias completos PT/EN para IA, ARM, shims, NXExtract, Mono Android, Godot, Cocos2d-x e testes; edição Unity selecionada com 15 casos e duas ferramentas genéricas; estudo de FP2 Vulkan→GLES2.

## Pendências de publicação

- Revisar editorialmente os guias nos dois idiomas e testar as receitas cross em ambiente independente com SDK/sysroot público pinado.
- Implementar demonstrações completas de loader Android, vídeo/áudio/input e NXExtract. O exemplo de shims existente demonstra somente seu contrato pequeno; o exercício NDK documentado não foi compilado nesta revisão.
- Revisar licenças e omissões por arquivo, especialmente Goblin Sword, PartyBoard, Pikmin, Forager e FP2; decidir termos para textos novos sem retirar direitos GPL/MIT existentes.
- Recuperar fonte exata/licença antes de importar código dos dois ports comunitários; não há URL pública de download verificada nesta coleção.
- Vincular cada referência histórica à versão fisicamente aprovada quando usada como prova final; HEAD público observado não basta.
- Medir cobertura de shims por contrato/engine; não prometer compatibilidade universal.
- Preparar CI pública independente se desejada. Doze testes históricos privados V5 ficaram fora da exportação; a suíte antiga depende de recursos não incluídos.

Não é necessário reconstruir os 45 ports para revisar fontes/documentação. Um novo executável, pin ou suporte exige a validação específica correspondente. Os verificados desta edição constam em [VALIDATION.md](VALIDATION.md).
