# Auditoria das referências aprovadas

Data da auditoria: 2026-08-08. Nenhum port existente foi alterado.

## Referências AArch64

`ports/bully2` e `ports/sonic4` (Sonic 4 Episode II) usam a mesma base comprovada
de `so_util` AArch64: mapeamento de múltiplos `PT_LOAD`, RELA, resolução de
imports, `init_array` e hook curto para trampolim. As diferenças específicas —
aliases C++ do Sonic e um filtro de construtor incompatível — demonstram por que
esses mecanismos precisam ser callbacks do adaptador, e não regras universais.
O Horizon Chase acrescenta três módulos Unity/IL2CPP e uma tabela grande o
suficiente para provar que a transação de relocation não pode ser quadrática.

Validação read-only do parser/relocator nos cinco ELFs reais aprovados:

| Referência | Segmentos | Símbolos | Relocações | `INIT_ARRAY` | Resultado local |
|---|---:|---:|---:|---:|---|
| Bully2 `libGame.so` | 3 | 50.356 | 75.033 | 506 | load + relocate OK |
| Sonic 4 EP2 `libfox.so` | 2 | 29.153 | 54.962 | 46 | load + relocate OK |
| Horizon `libunity.so` | 4 | 646 | 54.707 | 433 | load + relocate OK |
| Horizon `libil2cpp.so` | 4 | 2.658 | 305.750 | 24 | load + relocate OK |
| Horizon `libmain.so` | 4 | 13 | 20 | 0 | load + relocate OK |

Inventário exato das relocações AArch64:

| ELF | `RELATIVE` | `ABS64` | `GLOB_DAT` | `JUMP_SLOT` |
|---|---:|---:|---:|---:|
| Bully2 | 12.127 | 42.596 | 7.844 | 12.466 |
| Sonic 4 EP2 | 22.970 | 18.517 | 3.172 | 10.303 |
| Horizon Unity | 54.114 | 50 | 43 | 500 |
| Horizon IL2CPP | 303.401 | 1.631 | 152 | 566 |
| Horizon main | 9 | 0 | 0 | 11 |

SHA-256 dos guests inspecionados:

- Bully2: `345f6411bebbd2310b2cb5d2a271341847a054c2b7f6d5ffba8030cd53140d76`;
- Sonic 4 EP2: `ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a`;
- Horizon Unity: `7c46972d06b6b85f953bf61b477fdbf5bf22f7be55b57e4b54b4403f05478359`;
- Horizon IL2CPP: `acf414c77853aa0e7f8d8963f307f65f3d156c8d6d806661b738df3385e48cdc`;
- Horizon main: `a2dfe24e0170af0ae90b13630fc6d4e788be2c5216b4bddb17a6e0ca3c4c5af8`.

Todos são ELF64 little-endian, `ET_DYN`, `EM_AARCH64`, `e_flags=0` e usam
RELA com addend explícito. Nenhum contém `PT_INTERP`, TLS, IFUNC, TEXTREL,
RELR, packed relocations ou segmento W+X. O conjunto dinâmico observado é
somente `RELATIVE`, `ABS64`, `GLOB_DAT` e `JUMP_SLOT`. Não há `PREL`, `ADR`,
`ADRP`, `ADD`, `LDST`, `JUMP26` ou `CALL26`: o backend rejeita esses tipos
antes do callback, e patches de instrução comprovados continuam no adapter.

Os 1.009 slots de `INIT_ARRAY` foram inventariados e tiveram suas relocações
validadas, mas nenhum constructor, `JNI_OnLoad`, import ou código guest foi
executado. O filtro específico do Sonic e a ordem multi-módulo do Horizon
continuam responsabilidades do lifecycle do port.

Hosts Linux AArch64 aprovados, auditados separadamente do guest Android:

| Host Linux | SHA-256 | `PT_INTERP` | GLIBC máxima |
|---|---|---|---:|
| Bully2 `bully.glibc230` | `bd561ad5043e925c24e9fc45901a610df779da1005a43ac4c1e56588314ea228` | `/lib/ld-linux-aarch64.so.1` | 2.17 |
| Sonic 4 EP2 `sonic4.arm64` | `c02b250c640e9696718f89dfff217ec7c620be8b028b7c1ede6b60e3a719ea30` | `/lib/ld-linux-aarch64.so.1` | 2.27 |
| Horizon `horizonchase-universal` | `46a1da200f6e074568e9d1b5e5193b354cf5cbdd239dc7e8e967a26e43d91868` | `/lib/ld-linux-aarch64.so.1` | 2.27 |

Medição do inspector reconstruído a partir do core congelado nos dois modos
read-only. `--exports` reloca, cria o registry e chama `add_module` uma vez; não
resolve/finaliza nem executa lifecycle:

| Guest | `--relocate` | Exports adicionados | `--exports` | Pico RSS |
|---|---:|---:|---:|---:|
| Bully2 | 21.631.244 ns | 49.938 | 43.091.565 ns | 53.220 KiB |
| Sonic 4 EP2 | 19.933.161 ns | 28.776 | 22.834.337 ns | 53.220 KiB |
| Horizon Unity | 24.304.781 ns | 222 | 24.712.696 ns | 53.220 KiB |
| Horizon IL2CPP | 56.471.263 ns | 2.369 | 56.748.924 ns | 94.608 KiB |
| Horizon main | 883.585 ns | 1 | 711.194 ns | 87.280 KiB |

Os dez subprocessos guest adicionaram exatamente 81.306 exports, sem colisão ou
equivalência, e o gate completo levou 956.102.108 ns, incluindo hashes e a
validação estática.
Esses números são uma regressão de escala do host, não uma promessa de FPS ou
tempo de boot em device. RSS vem do `wait4` do filho e é um limite superior do
processo, inclusive o pico herdado antes de `exec`; não deve ser interpretado
como memória incremental do ELF. A tabela grande passou porque as escritas
pendentes são coletadas em O(n) e validadas por ordenação O(n log n), sem os
cerca de 46 bilhões de comparações que uma busca linear acumulada poderia
produzir. O parser também lê buckets/cadeia GNU em O(b+c) com budget comum e
valida duplicatas `DT_NEEDED` por heapsort O(n log n); fixtures com 32 buckets
compartilhados e 2.048 dependências cobrem ambos os ABIs.
Nomes dynstr têm teto de 4.096 bytes; 512 offsets sobrepostos validam o limite
sem scan ilimitado. Busca de `PT_LOAD` é binária e RELRO usa sweep linear,
testados com 4.096 segmentos e 4.096 relocações em cada ABI.
O registry também é um vetor ordenado com lookup binário `O(log n)` e ingestão
batch por heapsort `O((n+e) log(n+e))`; uma fixture com 2.048 nomes distintos que
compartilham os mesmos 12 bits baixos de FNV-1a impede o retorno do linear
probing quadrático. O batch preserva prioridade, weak/strong, duplicatas e
relatório na ordem pública, publicando tudo somente após sucesso integral.

A evidência final está presa ao snapshot ordenado dos 11 fontes do inspector
(`747ac4b6e60ed9527f73fe8526c4ef0bc29629391a3d530e91ae1c79e18a9b87`),
ao inspector reconstruído
(`a2f67c1f11b04e45f71ddee9473358ed13166a487fc7efa32356aa20cfd66723`),
ao JSON
(`32c9f88cb93a2f8cd915fcc3c7f7d1241a6dbd2c64cb5b34becbcbb55d9f48b4`)
e ao log
(`539bb6191766aff0ed322639e1d7c993d5585219115b9b3f6f45a9fd6ec52364`).
O supervisor usa deadline monotônico, limite de 256 KiB por stream, `pidfd` e
`wait4`; nove self-tests cobrem TERM→KILL, excesso de saída, coleta normal,
sanitização, inventário/relatório de exports, subprocesso `--exports` e
preservação de um processo irmão.

## Referências ARMv7

KOTOR fornece o caso aprovado de múltiplos módulos e softfp; o TASM2 canônico em
`ports/asm2_127` fornece o loader ARMv7 REL. A árvore abandonada `ports/tasm2`
permanece negative-only e não é fonte de implementação. Os GTA aprovados foram
usados como verificação da ordem load → relocate → resolve → proteção →
`init_array`, sem editar ou migrar esses ports.

Stardew Valley também foi cruzado somente como referência aprovada de launcher
fino, lifecycle e disciplina W^X/logging. Seu runtime atual é AArch64: ele não é
prova de ABI, relocação ou veneer ARMv7, e nenhum detalhe específico do jogo foi
copiado para o backend ELF32.

| Referência | Float ABI observada | Segmentos | Símbolos | Relocações | Resultado local |
|---|---|---:|---:|---:|---|
| KOTOR `libKOTOR.so` | `e_flags`: softfp | 2 | 17.640 | 24.072 | load + relocate OK |
| TASM2 1.2.7d `ports/asm2_127/.../libtasm2.so` | `e_flags`: unspecified | 2 | 1.235 | 74.970 | load + relocate OK |
| TASM2 `libgenerator.so` | `e_flags`: unspecified | 2 | 471 | 2.321 | load + relocate OK |

Inventário exato das relocações:

| ELF | `RELATIVE` | `ABS32` | `GLOB_DAT` | `JUMP_SLOT` |
|---|---:|---:|---:|---:|
| KOTOR | 320 | 10.971 | 3.228 | 9.553 |
| TASM2 principal | 74.548 | 15 | 20 | 387 |
| TASM2 generator | 2.237 | 0 | 4 | 80 |

SHA-256 dos guests inspecionados, mantidos somente como referência read-only:

- KOTOR: `099c43db23a891a19f40f5805c3582b390de39d96d6e50be52649a40498a68b5`;
- TASM2 principal: `d091fe95c56af681f1a06453e9868622935ecbb759c05c340fc16bf8df2ae62e`;
- TASM2 generator: `3871b9c4ae5d1c491abbccc7af1744121bf964f52b4812d2f672c72ff7c062ed`.

Nenhum desses três ELFs verdes contém `REL32`, `CALL`, `JUMP24`, `THM_CALL`
ou `IRELATIVE`. Portanto, os quatro tipos escalares históricos são comprovados
também por guest real; `REL32` e as branches são cobertos por fixtures sintéticas
normativas e continuam fail-closed/opt-in onde aplicável. Não alegamos que um
guest verde real já provou a implementação de branch.

O inventário dynsym pós-hardening contém somente `STT_FUNC`, `STT_OBJECT` e
`STT_NOTYPE`: KOTOR 14.426/3.210/4, TASM2 principal 1.112/95/28 e generator
280/187/4. Por isso a allowlist ARMv7 rejeita `STT_TLS` e qualquer outro tipo
antes do hook sem excluir símbolo necessário dessas três referências verdes.
As branches sintéticas `CALL/JUMP24/THM_CALL` não entram no `relocation_hook`:
usam somente o codec ARM/Thumb default validado. Customização de entry point é
uma etapa separada e explícita com `nxloader_module_install_hook()` pós-resolve.

KOTOR declara softfp e usa os wrappers `softfp_shim` aprovados. TASM2 omite os
dois bits de float ABI e seu adapter aprovado declara base AAPCS explicitamente;
o core não tenta adivinhar isso por nome ou section. O provider comum cobre a
tabela `libm` do KOTOR. O wrapper `difftime` do TASM2 permanece específico do
adapter porque também traduz a semântica LP32 do Bionic.

Os executáveis host ARMHF universais já aprovados também foram auditados:

| Host Linux | SHA-256 | `PT_INTERP` | GLIBC máxima |
|---|---|---|---:|
| KOTOR `kotor-universal` | `7a7f909dad8079a09d980073199cf3c21b2003b0287ba97ec04ed05273cc0245` | `/lib/ld-linux-armhf.so.3` | 2.27 |
| TASM2 `asm2_127-universal` | `b5558457f7d778146b744111aeee7e73447f08e83b33bcaa6000fd013e0631b0` | `/lib/ld-linux-armhf.so.3` | 2.27 |

Essa regra se aplica ao host Linux final. O `PT_INTERP` Android
`/system/bin/linker` presente no guest TASM2 é parte do APK original e não deve
ser confundido com o interpretador do loader Linux.

## O que foi generalizado

| Padrão legado comprovado | Contrato novo |
|---|---|
| estado global de um único `.so` | `nxloader_module` independente por módulo |
| descoberta por nomes de sections | `PT_DYNAMIC` e SysV/GNU hash, sem ler section headers |
| tabela linear/hash adversarial mesclada sem origem | providers nomeados, lookup binário e batch ordenado/atômico |
| fallback implícito para símbolos do processo | somente providers registrados pelo adaptador |
| unknown relocation podia apenas logar | falha atômica antes do callback; callback só para tipo allowlisted |
| skip/alias específico dentro de `so_util` | callbacks do port, core sem nome de jogo |
| pool de hook fora do tamanho validado | reserva explícita, bounds e alcance do branch verificados |
| proteção aproximada text/data | permissões por página, união de `PT_LOAD`, RELRO e W^X |
| parser confiava em offsets aditivos | checks de soma, multiplicação, arquivo, VMA e alvo de relocation |
| cada relocation varria todas as escritas anteriores | coleta O(n), validação O(n log n) e commit atômico |
| relocation podia reescrever sua própria metadata | `PT_DYNAMIC`, hashes, dynstr/dynsym e REL/RELA/PLT são ranges protegidos |
| cada bucket GNU percorria novamente a mesma cadeia | buckets uma vez + cadeia máxima uma vez, com budget O(b+c) |
| duplicata `DT_NEEDED` exigia comparação par a par | cópia privada + heapsort O(n log n), sem mudar a ordem pública |
| dynstr podia ser reescaneada até o fim por referência | nome limitado a 4.096 bytes em todos os consumidores guest |
| cada alvo/página varria todos os `PT_LOAD` | busca binária por VMA + sweep RELRO linear |
| hash de exports podia receber colisões guest e degradar para `O(n²)` | registry ordenado, lookup `O(log n)` e batch `O((n+e) log(n+e))` |
| callback host podia reentrar antes do commit externo | guard por módulo, violação sticky e `NXLOADER_EREENTRANT` para log/alias/hook/filter |
| `relocation_hook=SKIP` podia ocultar strong ausente e deixar `JUMP_SLOT` em PLT0 | `SKIP` exige default comprovado; sem default conta `unresolved_strong`, falha atomicamente e permite retry |
| filtro e execução de constructors eram intercalados | plano integral `DT_INIT` → `INIT_ARRAY` antes de qualquer código guest |
| cada adapter fazia lookup/chamada JNI sem fronteira comum | API opt-in, lookup literal, JavaVM/reserved validados e allowlist exata antes de `READY` |

## Limite da evidência

“Load + relocate OK” prova o parser, o mapeamento e as relocações locais contra os
ELFs reais. Não significa que o novo núcleo já substituiu o loader desses jogos:
imports, shims, JNI, render, áudio, controles, save e saída continuam exigindo a
integração e os testes físicos do port-piloto. Por isso os ports aprovados ficaram
intocados e servem apenas como referência verde.

O teste de cache AArch64 executa o mesmo endereço antes e depois do patch e deixa
somente `nxloader_module_finalize()` invalidar a I-cache, mas roda em QEMU. Isso
prova a integração do loader e não substitui evidência de coerência em hardware
AArch64 físico; `physical_device_evidence` permanece falso neste marco.

Os ELFs dos jogos não fazem parte de `nxloader`, não entram nos testes sintéticos
e não devem ser adicionados a uma release do framework.

Na API 1.3, callbacks host continuam confiáveis e não reentrantes, mas a
pré-condição agora é aplicada defensivamente a `log`, `alias`,
`relocation_hook` e `initializer_filter`: qualquer API sobre o mesmo módulo é
bloqueada, `destroy` não libera o contexto e a violação sticky força a chamada
externa a retornar `NXLOADER_EREENTRANT` sem commit. Outro módulo permanece
acessível. Os estados transitórios protegem execução guest, porém isso não torna
constructor ou JNI transacional depois que código guest começou; falha, loop ou
efeito externo dentro do guest continua responsabilidade do adapter/lifecycle.

Os testes M11 usam somente ELFs sintéticos e filtros `SKIP`/`REJECT`: cobrem os
quatro callbacks, ambos os backends, preflight e a API JNI no caminho ausente.
Nenhum constructor ou `JNI_OnLoad` real foi executado, e não há nova evidência
de device. A ordem nativa observada nos adapters aprovados permanece
constructors antes de JNI; o novo core apenas torna essa fronteira explícita e
fail-closed. Em fluxos multi-módulo, o provider ainda deve sobreviver ao registry
e a todos os consumidores que receberam seus endereços. Destruir o módulo não
invalida o registry; a ordem de teardown é consumidores → registry → providers.

## Builds de validação

- host x86_64: GCC e Clang com `-Wall -Wextra -Wpedantic -Werror`;
- host x86_64: ASan + UBSan e analisador estático Clang sem achados;
- host x86_64: 2.048 mutações determinísticas e 20.000 execuções libFuzzer
  instrumentando o núcleo; preflight sintético usa filtros `SKIP`/`REJECT`, sem
  executar initializers ou JNI guest;
- AArch64: probes LP64/AAPCS64/cache executados em QEMU; build Debian Buster
  exige no máximo `GLIBC_2.17`;
- ARMv7 hardfp: teste sintético executado em QEMU; core/teste exige no máximo
  `GLIBC_2.4`;
- módulo softfp ARMv7: provider compilado e testado; executável de teste exige
  no máximo `GLIBC_2.27`.

O gate ARMHF M09 recompila o núcleo e o provider em GCC e Clang/LLD, roda os
probes ABI e os testes do provider nos dois builds em QEMU e audita 26 ELFs
temporários (7 loadables e 19 relocatables). Todos são
ELF32 little-endian, `EM_ARM`, EABI5 e hardfp conforme o tipo; os executáveis
usam `/lib/ld-linux-armhf.so.3`, não há RPATH/RUNPATH/TEXTREL/GLIBC_PRIVATE e a
maior versão observada é `GLIBC_2.27`. O probe chama wrappers `float` e `double`
reais, confirma stack pública alinhada a 8 bytes e preservação de `d8`. Ele não
carrega guest nem chama initializer.

O gate AArch64 M10 recompila o core com GCC 8.3 e Clang/LLD sobre o mesmo
sysroot Debian Buster, audita 20 ELFs temporários (5 loadables e 15
relocatables) e executa os dois probes em QEMU. Ele confirma LP64, argumentos
inteiros/FP, SP público alinhado a 16 bytes, preservação de `x19–x28` e dos 64
bits inferiores de `d8–d15`, cache RW→RX e ausência de RWX. Nos dois builds, um
ELF sectionless pertencente ao teste aplica uma `RELATIVE`, resolve com registry
vazio, executa entry/pool para primar cache, volta RX→RW sem RWX, instala o
veneer público, finaliza sem clear manual intermediário e reexecuta os dois
endereços já usados; nenhum initializer/JNI ou guest externo é chamado. Todo
executável usa
`/lib/ld-linux-aarch64.so.1`; a maior versão observada é `GLIBC_2.17`.

Esses executáveis foram gerados somente em diretórios temporários para validar o
código. Nenhum binário compilado foi adicionado ao repositório ou a pacote de
release.
