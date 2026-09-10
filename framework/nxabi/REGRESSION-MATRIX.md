# nxabi — matriz de regressão

Cada linha é uma garantia que já quebrou (ou quase quebrou) uma vez e agora
tem prova executável. Rodar tudo:

```
python3 -B framework/nxabi/tests/test_nxabi.py
python3 -B framework/nxabi/tests/test_sdl_authority.py
python3 -B framework/nxabi/tests/test_v3_roles.py
python3 -B framework/nxabi/tests/test_m17_closure.py
bash framework/nxabi/tools/nx-abi-gate.sh
```

## 0.2.3 — paridade integral do piso (V4-05A)

| Fronteira | Falha que deve fechar | Prova |
|---|---|---|
| decisão única | nxabi e nxrelease divergirem em severidade para o mesmo símbolo (allowed/pós-piso/desconhecido/indireto/SDL3/waiver/PUBLIC) | `nxrelease/tests/test_sdl_floor_parity.py` (7 classes) |
| símbolo desconhecido | `sdl-unknown` virar warning em candidato público | idem, classe 3 |
| waiver | assinatura histórica rebaixar `sdl-floor`/`sdl-unknown` públicos ou readmitir Vendor/Product | idem, classe 6 |

## 0.2.2 — autoridade única de símbolo SDL (V4-03B)

| Garantia | Prova |
|---|---|
| A tabela `sdl2-symbol-floor.tsv` se identifica (`#% authority: nx-sdl-symbol-floor/1`) e o recibo do audit carrega id + SHA-256 dos bytes exatos | `AuthorityTableTest` |
| Tabela ausente, symlink, não-UTF-8, linha malformada, símbolo/versão inválidos, duplicata ambígua, id errado ou tabela vazia falham fechado — nunca viram `{}` que aprova tudo | `StrictParserTest` |
| Policy sem `sdl.table` reprova em vez de desligar o piso silenciosamente | `test_missing_policy_table_fails` |
| Vendor/Product (SDL 2.0.6) reprovam acima do piso 2.0.4 nos DOIS consumidores, e o nxrelease abre exatamente a mesma autoridade (id, hash e mapa integral) | `ConsumerConsistencyTest` |

## 0.2.1 — semântica do piso preferido

| Garantia | Prova |
|---|---|
| Wrapper introduzido entre GLIBC 2.17 e 2.30 permanece visível como aviso, sem transformar a preferência em teto | `test_new_libc_wrapper_within_public_ceiling_warns` |
| Wrapper posterior a GLIBC 2.30 continua falhando fechado | `test_new_libc_wrapper_above_public_ceiling_fails` |

## M17 (0.1.0)

| Garantia | Prova |
|---|---|
| Tetos GLIBC 2.30 / GLIBCXX 3.4.25 / CXXABI 1.3.11; preferido 2.17 | `test_nxabi.py::PolicyTest` |
| `GLIBC_PRIVATE`/`GLIBC_ABI_*` reprovam sempre | `test_policy_glibc_private_fails` |
| Waiver por sha256: rebaixa e nunca esconde; morre no rebuild | `test_waiver_*` |
| `nx_symver.h` derruba o piso 2.27 → 2.4 (medido em ELF real) | `RealElfTest` |
| Pin de toolchain: drift ou ausência reprova o comando release | `test_m17_closure.py` (`if status != "ok":`) |
| Passo report-only honesto em árvore parcial | `nx-abi-gate.sh` passo 6 + guard do closure test |

## V3-ABI-01 (0.2.0)

| Garantia | Prova (`tests/test_v3_roles.py`) |
|---|---|
| Enum de papéis é exatamente guest/loader/extractor/splash/adapter/helper | `test_enum_is_exactly_the_v3_contract` |
| Papel declarado é validado, nunca adivinhado; enum desconhecido reprova | `test_declared_role_is_validated_not_guessed` |
| `game` (nxrelease) ≡ `guest` (nxabi) — vocabulários compatíveis | `test_nxrelease_game_alias_normalizes_to_guest` |
| Convenções: `nxsplash*`→splash, `nxextract-ui*`→extractor | `test_documented_conventions` |
| `*-nextos` = loader/guest SÓ por declaração; nome sozinho falha fechado | `test_nextos_suffix_needs_a_declaration` |
| Helper JAMAIS herda a ABI do jogo/host implicitamente | `test_helper_never_inherits_the_game_abi` |
| ABI do papel ≠ ABI do host NÃO é erro (fecho misto por papel) | `test_role_abi_may_differ_from_the_host_abi` |
| Fecho misto de 3 ABIs passa quando cada papel é autoconsistente | `test_mixed_three_abi_closure_passes_when_self_consistent` |
| Terceira ABI exige class+interpreter declarados (nada por costume) | `test_third_abi_needs_explicit_declaration` |
| Cada papel validado em isolamento | `test_each_role_is_validated_in_isolation` |
| Interpreter ausente = falha NOMEADA por papel | `test_missing_interpreter_is_a_named_failure` |
| Nenhuma SONAME presumida (libudev incluída); só o que o chamador declara | `test_nothing_is_assumed_by_custom` |
| GNU ld script classificado `linker-script`, nunca "invalid ELF" | `test_ld_script_is_linker_script_not_invalid_elf`, `test_require_elf_never_calls_a_script_invalid_elf` |
| ELF truncado/lixo/symlink/texto têm classes próprias | `test_text_symlink_truncated_and_junk` |
| GROUP + AS_NEEDED resolve ordenado com origem por membro | `test_group_with_as_needed_resolves_in_order` |
| Membro absoluto re-enraizado no sysroot; escapar dos roots reprova | `test_absolute_member_is_rerooted_inside_the_sysroot`, `test_traversal_outside_the_search_roots_fails` |
| Membro não resolvível = fail-closed | `test_unresolvable_member_fails_closed` |
| Backtick/metacaractere de shell no script = rejeição (defesa) | `test_backticks_and_shell_metacharacters_are_rejected` |
| Bomba de recursão (profundidade > 4 / ciclo) = rejeição | `test_recursion_bomb_is_rejected` |
| Script aninhado dentro do limite resolve e propaga AS_NEEDED | `test_nested_script_within_depth_resolves` |
