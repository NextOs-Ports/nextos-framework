# nxabi — gate de toolchain, ABI e baixa glibc (M17)

`nxabi` é a fonte única de verdade dos tetos de ABI do projeto e o auditor de
todo ELF Linux que o projeto constrói. É host-side e **read-only**: nunca
executa o ELF inspecionado, nunca carrega guest code e nunca fala com aparelho.

```
python3 -B framework/nxabi/nxabi.py audit ports/<port>/<binario>
python3 -B framework/nxabi/nxabi.py inventory ports/<port> --json inv.json
python3 -B framework/nxabi/nxabi.py toolchain
bash framework/nxabi/tools/nx-abi-gate.sh          # o gate completo
```

## Arquivos

| Arquivo | Papel |
|---|---|
| `policy-v1.json` | tetos, allowlist de SONAME, blacklist de wrappers, waivers |
| `TOOLCHAIN-PIN.json` | toolchains pinadas (compilador, sysroot, sha256, imagem+digest) |
| `sdl2-symbol-floor.tsv` | **autoridade única** símbolo SDL → versão mínima (`#% authority: nx-sdl-symbol-floor/1`; consumida pelos MESMOS bytes por nxabi e nxrelease; parser estrito fail-closed) |
| `include/nx_symver.h` | derruba o piso de `GLIBC_2.27` para `2.4`/`2.17` |
| `tools/nx-abi-gate.sh` | gate único do M17 |
| `tools/check-determinism.sh` | duplo build com exigência de sha256 idêntico |
| `m17-reference-audit-v1.json` | evidência executada sobre os ports de referência |
| `m17-closure-v1.json` | fechamento M17-001..020 com evidência, garantia, limite e teste |

O recibo de fechamento é validado sem executar ELF ou acessar hardware:

```sh
python3 -B framework/nxabi/tests/test_m17_closure.py
```

## Tetos (M17-003, M17-010)

| Namespace | Teto público | Preferido |
|---|---|---|
| `GLIBC` | 2.30 | **2.17** |
| `GLIBCXX` | 3.4.25 | — |
| `CXXABI` | 1.3.11 | — |

`GLIBC_PRIVATE` e `GLIBC_ABI_*` reprovam sempre. Os tetos vivem em
`policy-v1.json`; `nxrelease.py` mantém os mesmos valores gravados no código
como lei imutável e **falha se a policy divergir** (`assert_abi_policy_agrees`).
`framework/nxloader/tools/check-glibc.sh` lê o teto daqui.

Desde a 0.2.1, a auditoria de wrappers segue a mesma distinção: uma API glibc
posterior a 2.17, mas introduzida até 2.30 inclusive, gera aviso com a elevação
do piso; uma API posterior a 2.30 reprova. Assim o piso preferido continua uma
meta explícita e o teto público continua a fronteira obrigatória.

## Piso preferido e o `nx_symver.h` (M17-002)

Todo artefato universal medido estava em `GLIBC_2.27` por um motivo só: a glibc
2.27 republicou as entradas de precisão simples da `libm` e o linker escolhe a
versão mais nova que enxerga.

```
ports/kotor/kotor-universal          exp2f expf log2f logf powf @GLIBC_2.27
ports/horizonchase/horizonchase-universal              powf @GLIBC_2.27
ports/sonic4/sonic4.arm64                             powf @GLIBC_2.27
```

As versões antigas continuam exportadas pela mesma `libm` dos devices, então
incluir `nx_symver.h` (com os `-fno-builtin-<fn>` correspondentes) derruba o
piso. Medido em fixture próprio:

| ABI | Sem o header | Com o header |
|---|---|---|
| ARMHF (GCC 8.2.1, sysroot 2.28) | `GLIBC_2.27` | **`GLIBC_2.4`** |
| AArch64 (container buster pinado) | `GLIBC_2.27` | **`GLIBC_2.17`** |

> ⚠️ **Nenhum binário aprovado foi regerado.** Trocar o piso muda os bytes do
> artefato e exige revalidação física do NextOS (regras #22/#33). O header está
> pronto; aplicar em port publicado é decisão do NextOS.

## Waivers (exceções auditáveis)

Um waiver em `policy-v1.json` é chaveado pelo **sha256 do artefato**, então
morre no instante em que o binário é reconstruído. Ele nunca esconde o achado:
rebaixa de `ERROR` para `WARN` e imprime o motivo. Só existe waiver para
artefato com evidência física registrada no catálogo.

O `RUNPATH=[$ORIGIN]` do `horizonchase-universal` **não** tem waiver de
propósito: é a questão de política aberta em `open_policy_questions`, e o
padrão adotado é o estrito (o `nxrelease` já reprova qualquer RPATH/RUNPATH).

## Escopo fechado e dívidas históricas

Cobre, sem hardware: pin de toolchain, ABI/class/endian/float, PT_LOAD,
PT_INTERP, PT_GNU_STACK, DT_NEEDED/SONAME/RPATH/RUNPATH, tetos GLIBC/GLIBCXX/
CXXABI, `GLIBC_PRIVATE`, separação Android×Linux, wrappers novos de libc, piso
SDL, build-id, proveniência de toolchain e duplo build determinístico.

O M17 está fechado para o framework e para todo artefato público novo: o gate só
aceita toolchains pinadas presentes localmente, exige o build determinístico de
fixture próprio e falha em qualquer ELF público acima dos tetos. O relatório dos
cinco ports aprovados é deliberadamente `report-only`: binários históricos não são
rebatizados como conformes e nunca entram como referência de empacotamento quando
falham a policy.

Continuam como dívida externa, sem enfraquecer esse fechamento:

- **receitas aprovadas históricas ainda não herméticas** —
  `ports/kotor/build_universal.sh`, `ports/asm2_127/build_buster_arkos.sh` e
  `ports/bully2/build-glibc230.sh` não são entradas aceitas pelo gate. Migrar
  exige toolchain offline pinada e **rebuild**, que muda os bytes do artefato
  aprovado. Estão listadas em
  `TOOLCHAIN-PIN.json → non_hermetic_recipes`.
- **`suportando_outros_devices/extrator-universal/tools/check-glibc.sh`** ainda
  tem o teto gravado no código e não pega `GLIBC_PRIVATE`. Não foi tocado
  porque o arquivo estava sob edição de outra sessão (bump 1.2.5 → 1.2.6).
- **transição do catálogo para `executed`** — `framework/catalog/abi-checks-v1.tsv`
  é **gerado** por `generate-abi-checks.py` com `status=recorded` fixo; editar à
  mão quebra `framework/catalog/tests/test_catalog.py`. A evidência executada
  sai em `m17-reference-audit-v1.json`, que referencia os IDs do catálogo.

## V3 (0.2.0) — execution roles e GNU ld scripts (V3-ABI-01)

Aditivo ao M17; nada acima mudou de semântica. Compatível com o modelo do
`nxrelease.validate_execution_role_elfs` (`game` de lá ≡ `guest` daqui).

**Execution roles.** Todo executável do pacote tem um papel:
`guest | loader | extractor | splash | adapter | helper`.
Regras: cada papel carrega a **própria** ABI declarada — um helper jamais
herda a ABI do jogo/host; fecho misto (host AArch64 + guest ARMHF + splash de
terceira ABI) é válido quando cada papel é autoconsistente. Convenções de nome
documentadas (fora delas, só por declaração, fail-closed):
`nxsplash*`→splash, `nxextract-ui*`→extractor, `*-nextos`→loader/guest **por
declaração**.

```python
classify_execution_role("nxsplash-armhf")            # -> "splash"
classify_execution_role("chrono-nextos", "loader")   # -> "loader"
validate_role_abi("helper", elf_info, host_abi)      # findings por papel
audit_mixed_closure(roles, physical_libs, firmware_contract)
```

`audit_mixed_closure` exige que todo DT_NEEDED de todo papel resolva no
conjunto físico embarcado ou no contrato de firmware **declarado pelo
chamador** — nenhuma SONAME (libudev incluída) é presumida por costume;
interpreter ausente é falha nomeada por papel.

**GNU ld scripts.** `classify_linker_input(path)` →
`elf | linker-script | text | symlink | invalid`: o `libc.so` de sysroot
(`GROUP ( libc.so.6 AS_NEEDED ( ld-linux-armhf.so.3 ) )`) é `linker-script`,
nunca "invalid ELF". `resolve_linker_script(path, search_paths)` expande
GROUP/INPUT/AS_NEEDED contra roots estilo sysroot e devolve o fecho ordenado
com origem por membro; reprova fechado membro não resolvível, recursão > 4,
traversal para fora dos roots e script com metacaracteres de shell/backticks.

Prova: `python3 -B framework/nxabi/tests/test_v3_roles.py` (também roda no
passo 2 do gate). Matriz completa em `REGRESSION-MATRIX.md`; histórico em
`CHANGELOG.md`.
