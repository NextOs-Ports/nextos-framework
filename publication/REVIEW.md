# Revisão antes de publicar

[English](REVIEW.en.md)

Ports, fontes, estudos e exemplos iOS foram excluídos da seleção atual. Declarações multiplataforma em dependências genéricas, como SDL, permanecem nas fontes preservadas. A remoção ocorreu por commit normal: o histórico privado anterior ainda contém a seleção antiga. Antes de tornar público, revisar também qual histórico será publicado.

O mantenedor precisa aprovar explicitamente a mudança de visibilidade. Esta preparação continua privada.

A [atualização do primeiro port](ONBOARDING-UPDATE.md) registra a implementação das lacunas apontadas pela [simulação anterior](ONBOARDING-SIMULATION.md), com testes e limites explícitos.

A [revisão das ferramentas V5 de 12/09/2026](V5-REVIEW-20260912.md) acrescenta conferência exata das árvores congeladas, correções de inventário/recuperação e CI automático da coleção, com 48 testes host aprovados. Preserva o runtime V5 e não incorpora V6. O workflow completo do SDK continua sendo uma verificação manual separada.

## Incluído nesta edição

V5 preservada; fontes de 40 repositórios/44 títulos; duas fichas comunitárias adicionais; guias PT/EN; edição Unity selecionada com 15 casos e ferramentas genéricas; estudo FP2 Vulkan→GLES2. Agora inclui SDK público, pins da coleção, minijogo integrado com nxloader/NXExtract, inventário executável, perfis pesquisáveis, exercícios de engines e laboratório de shaders.

## Pendências de publicação

- Revisar os guias nos dois idiomas e acompanhar a primeira execução independente do workflow manual de CI. O SDK público já foi construído e os testes de CPU/extração do exemplo passaram localmente.
- Validar o exemplo integrado em aparelho autorizado: UI NXExtract, NXSplash, pixels, som, controles, save e saída. Seus contratos gerados continuam não release. Os builds de editor Unity, integração Cocos e exercício NDK separado ainda estão pendentes.
- Recuperar a origem exata do pin SPIRV-Cross histórico do FP2 antes de alegar rebuild idêntico. O laboratório de shaders usa outro pin público explícito; não substitui a receita aprovada.
- Revisar licenças e omissões por arquivo, especialmente PartyBoard, Pikmin, Forager e FP2; decidir termos para textos novos sem retirar direitos GPL/MIT existentes.
- Recuperar fonte exata/licença antes de importar código dos dois ports comunitários; não há URL pública de download verificada nesta coleção.
- Vincular cada referência histórica à versão fisicamente aprovada quando usada como prova final; HEAD público observado não basta.
- Medir cobertura de shims por contrato/engine; não prometer compatibilidade universal.
- Doze testes históricos privados V5 ficaram fora da exportação; o novo CI didático não restaura nem certifica essa suíte antiga.

Não é necessário reconstruir os 44 ports para revisar fontes/documentação. Um novo executável, pin ou suporte exige a validação específica correspondente. Consulte os [testes da atualização](ONBOARDING-UPDATE.md) e a [validação editorial anterior](VALIDATION.md).
