# 0.9.0 (2026-08-30, M124: AArch64 I-cache attestation)

- A atestação AArch64 passa a exercer o fluxo sintético real já existente:
  a mesma mapping owner percorre `finalize → A → begin_patch → rewrite de
  entry/veneer → finalize → B`, sem `mprotect` positivo do harness. Mapping,
  entry e pool preservam os VAs; a entry cruza uma página e cache line medidas.
- `nxloader_module_begin_patch()` é aditiva: somente `FINALIZED` antes dos
  initializers entra em `PATCHING`; páginas executáveis patchable passam a RW,
  nunca RWX, enquanto RELRO e pools auxiliares permanecem fechados, e outro
  `finalize()` é obrigatório. Falha parcial tenta restaurar as proteções finais
  e publica `ERROR` se essa restauração falhar.
- O runner emite `COMPILED_ONLY/PENDING` sem execução ou `EMULATED/PASS` por
  QEMU, preserva os binários owner-local fora da árvore e recusa receipt stale,
  truncado, duplicado, adulterado, com dados privados ou classe forjada.
  `PHYSICAL` permanece `PENDING` e não emitível sem trust anchor externo.
- O componente passa a `0.9.0`, preservando API `1.3`, valores, layouts e todas
  as assinaturas 0.8.0. A closure 0.8.0 permanece byte-intacta e nenhum port é
  migrado automaticamente.

# 0.8.0 (2026-08-29, V4-GRAPHICS-03: nenhum construtor sobre GOT cru)

- `nxloader_module_call_initializers` passa a recusar com `NXLOADER_EUNRESOLVED`
  enquanto restar qualquer import **forte** indefinido do último `resolve`.
  `NXLOADER_RESOLVE_ALLOW_UNRESOLVED` continua existindo como auxílio de
  bring-up/inspeção, mas **deixa de autorizar execução de código do guest**.
- Esse era exatamente o estado do incidente OTR 1.0.3 no ROCKNIX: o resolver
  imprimia `UNRESOLVED`, os construtores rodavam mesmo assim e o primeiro
  `eglGetCurrentContext@plt` saltava pelo valor link-time cru do GOT.
  `JNI_OnLoad` já dependia de `INITIALIZED` e portanto fica fechado junto.
- O contador de imports fortes indefinidos passa a ser mantido pelo módulo,
  mesmo quando o chamador não pede relatório.
- `tests/test_nxloader.c` prova a recusa nas duas ABIs e confirma que o slot
  continua intocado.

# Changelog do nxloader

## 0.7.2 — 2026-08-13

- `NXLOADER_RELOC_SKIP` agora exige que o backend ou um provider tenha
  produzido um default comprovado. Em import strong ausente, `SKIP` preserva o
  slot apenas como não resolvido, incrementa `unresolved_strong` e mantém a
  falha atômica padrão.
- `NXLOADER_RELOC_WRITE` continua sendo resolução explícita mesmo sem provider;
  `SKIP` com default e o opt-in diagnóstico
  `NXLOADER_RESOLVE_ALLOW_UNRESOLVED` preservam seus contratos.
- Regressões ELF32/ARMv7 e ELF64/AArch64 cobrem `JUMP_SLOT` strong iniciado com
  sentinela PLT0, atomicidade, default/provider, weak A/zero, retry, `WRITE`,
  `SKIP` e `ALLOW_UNRESOLVED`.
- A unidade de hooks declara o feature-test macro antes dos headers do sistema,
  mantendo `MAP_ANONYMOUS` disponível também nos gates cross com `-std=c99` e
  `-Werror`.

## 0.7.1 — 2026-08-12

- Destinos escalares AArch64 desalinhados aceitos pelo linker Android usam
  acesso por `memcpy`, preservando bounds, ranges protegidos e sobreposição.

## 0.7.0 — 2026-08-12

- Pools auxiliares AArch64 fora do alcance direto ganharam suporte opt-in com
  bounds e capacidade transacionais.
