# 0.2.3 (2026-09-01, paridade integral do piso SDL — V4-05A)

- `decide_sdl_floor()` vira A decisao unica do piso SDL2-core, consumida por
  nxabi e nxrelease: o mesmo simbolo jamais pode ser warning numa ferramenta
  e fatal na outra.
- Simbolo ausente da autoridade passa a FALHAR FECHADO (`sdl-unknown` =
  error) para perfil publico/universal: um simbolo nao provado nao cumpre o
  piso. Perfis nao publicos preservam o warning.
- Waiver historico nunca rebaixa o veredito de candidato publico/universal:
  os checks `sdl-floor`/`sdl-unknown` sao NAO-waivaveis em perfil publico
  (o waiver permanece legivel como metadata, com recusa nomeada); Vendor/
  Product (SDL 2.0.6) jamais reentra acima do piso 2.0.4 por assinatura
  antiga.
- O alcance indireto ja existente (import SDL_* sem DT_NEEDED direto do
  core) permanece; a exclusao documentada de SDL3 permanece.

# Changelog — framework/nxabi

## 0.2.2 — 2026-08-31 (autoridade única de símbolo SDL, V4-03B)

- `sdl2-symbol-floor.tsv` passa a ser a autoridade única e versionada de
  símbolo SDL → versão mínima, identificada pelo directive
  `#% authority: nx-sdl-symbol-floor/1`. O gerador `sdl-table` emite o
  directive; o parser novo `load_symbol_authority()` é estrito e falha
  fechado para tabela ausente, symlink, não-UTF-8, linha malformada, símbolo
  inválido, versão inválida, duplicata ambígua, id de autoridade errado ou
  tabela vazia — nunca degrada para um mapa vazio que aprova tudo.
- `load_sdl_table()` deixa de retornar `{}` silencioso quando a policy não
  declara tabela ou o arquivo falta: agora reprova. Estar numa allowlist de
  SONAME nunca equivale a cumprir o piso; a decisão é sempre pela versão de
  nascimento na autoridade.
- O relatório do `audit` registra o recibo da autoridade consumida:
  id, SHA-256 dos bytes exatos, contagem de símbolos e piso efetivo — o
  mesmo recibo que o `nxrelease` 0.3.22 grava no preflight, tornando a
  concordância dos dois consumidores verificável byte a byte.
- Waivers continuam chaveados por SHA-256 de artefato e apenas rebaixam
  `sdl-floor` para WARN visível em artefato histórico já aprovado; um port
  novo (bytes novos) jamais herda waiver.
- Gate novo `tests/test_sdl_authority.py`: parser estrito, identidade da
  tabela, caso de campo Vendor/Product (2.0.6 > piso 2.0.4) reprovando nos
  DOIS consumidores e prova de que o nxrelease abriu exatamente a mesma
  autoridade (id, hash e mapa integral).

## 0.2.1 — 2026-08-30 (piso preferido não é teto)

- Corrige M17-015 para respeitar os dois limites publicados: wrappers glibc
  introduzidos até `GLIBC_2.30` geram aviso por elevarem o piso preferido 2.17;
  somente wrappers posteriores a 2.30 reprovam.
- Mantém visíveis símbolo, versão de introdução, piso preferido e teto em todo
  achado, sem waiver e sem relaxar GLIBC/GLIBCXX/CXXABI máximas.
- Regressão cobre `memfd_create` (2.27, aviso) e `close_range` (2.34, erro).

## 0.2.0 — 2026-08-26 (V3-ABI-01)

Aditivo; nenhuma verificação existente mudou de semântica. `policy-v1.json`,
`m17-closure-v1.json` e `m17-reference-audit-v1.json` intocados.

- **Modelo de execution roles** (`EXECUTION_ROLES`, `classify_execution_role`,
  `validate_role_abi`): cada papel (`guest`, `loader`, `extractor`, `splash`,
  `adapter`, `helper`) carrega a própria ABI declarada — um helper JAMAIS herda
  a ABI do jogo/host implicitamente. Fechos mistos (host AArch64 + guest ARMHF
  + splash de uma terceira ABI) são válidos quando cada papel é
  autoconsistente. Vocabulário compatível com o
  `validate_execution_role_elfs` do nxrelease (`game` é alias de `guest`).
  Convenções documentadas: `nxsplash*`→splash, `nxextract-ui*`→extractor,
  `*-nextos`→loader/guest só por declaração; fora disso, fail-closed.
- **Reconhecimento de GNU ld script** (`classify_linker_input`,
  `require_elf`): um arquivo com gramática de ld script (`GROUP(...)`,
  `INPUT(...)`, `OUTPUT_FORMAT(...)`, comentários `/* */`) é classificado
  `linker-script` — nunca mais reportado como "invalid ELF". Classes:
  `elf | linker-script | text | symlink | invalid`.
- **`resolve_linker_script`**: expande GROUP/INPUT (com `AS_NEEDED(...)`)
  contra search paths estilo sysroot, retornando o fecho ordenado com origem
  por membro. Fail-closed em: membro não resolvível, recursão > 4, traversal
  para fora dos roots (membro absoluto é re-enraizado no sysroot) e script com
  metacaracteres de shell/backticks.
- **`audit_mixed_closure`**: todo DT_NEEDED de todo papel precisa resolver no
  conjunto físico enviado ou no contrato de firmware DECLARADO pelo chamador;
  interpreter ausente é falha nomeada por papel; nenhuma SONAME (libudev
  incluída) é presumida por costume.
- **Testes**: `tests/test_v3_roles.py` (29 casos, fixtures sintetizadas com
  tempfile; runner standalone com linha final PASS distintiva), integrado ao
  passo 2 do `tools/nx-abi-gate.sh`.
- `TOOL_VERSION`/`VERSION` 0.1.0 → 0.2.0. Documentação: seção V3 no README e
  `REGRESSION-MATRIX.md`.

## 0.1.0 — fechamento M17

Gate de ABI/toolchain M17-001..020: inventário/auditoria de ELF, tetos
GLIBC/GLIBCXX/CXXABI, pin de toolchain, piso SDL, proveniência, determinismo.
