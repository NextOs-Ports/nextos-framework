# nxloader

`nxloader` é o núcleo estático e versionado dos novos **so-loaders Android** do
NextOS. Ele carrega módulos `ET_DYN` ARMv7 ou AArch64, aplica as relocações
comprovadas nos ports aprovados, resolve imports por um registro explícito e
mantém cada `.so` em um contexto independente. A biblioteca é ligada dentro do
loader final; não é um `.so` adicional distribuído no pacote.

Versão atual: `0.9.0`; API pública preservada: `1.3`.

O objetivo é substituir cópias divergentes de `so_util`, não substituir o fluxo
nativo do jogo. O core oferece uma fronteira explícita e opt-in para despachar
`JNI_OnLoad`; JavaVM, versões JNI aceitas, decisão de presença obrigatória,
Activity/lifecycle, EGL, GLES, áudio e input continuam pertencendo ao adaptador
do jogo e aos demais módulos do framework.

## Contrato principal

O fluxo é deliberadamente explícito:

1. `nxloader_module_load_*()` valida o ELF e mapeia todos os `PT_LOAD`.
2. `nxloader_module_relocate()` aplica somente relocações locais/relativas.
3. `nxloader_module_resolve()` resolve imports no registro informado.
4. Hooks de entrada opcionais são instalados antes da proteção final.
5. `nxloader_module_finalize()` limpa cache de instruções, aplica permissões por
   segmento e GNU RELRO.
6. Antes de qualquer initializer, um adapter que precise substituir uma entry
   já executada pode chamar `nxloader_module_begin_patch()`: a mesma mapping e
   os mesmos VAs passam de RX para RW, nunca RWX, entram em `PATCHING`, recebem
   o hook/veneer e exigem outro `finalize()` antes de qualquer execução.
   Antes da chamada, o adapter confiável deve parar e confirmar todas as threads
   capazes de executar esse módulo e mantê-las quiescentes até o segundo
   `finalize()` concluir; o core não adivinha nem suspende threads do guest.
   RELRO e páginas sem execução conservam as permissões finais. Pools auxiliares
   de far hooks ficam RX e só reabrem internamente para a escrita limitada do
   slot, voltando a RX sem qualquer janela RWX.
7. `nxloader_module_call_initializers()` primeiro valida e filtra o plano
   completo e só então chama `DT_INIT` seguido de `DT_INIT_ARRAY`, exatamente
   uma vez e em ordem.
8. Quando o fluxo Android comprovado exige JNI, o adaptador chama
   `nxloader_module_call_jni_onload()` com JavaVM não nula e uma allowlist exata
   de versões; ausência só é sucesso com `NXLOADER_JNI_ONLOAD_OPTIONAL`.
9. O adaptador continua o restante do lifecycle Android real no ponto nativo.

Nenhuma fase posterior é chamada implicitamente e os construtores nunca são
pulados por padrão. Um filtro de construtor existe somente para incompatibilidade
comprovada e deve ser instalado explicitamente na configuração do port.

Um contexto pode ser usado por uma thread de cada vez. Módulos diferentes não
compartilham estado e podem ser coordenados pelo adaptador, mas registro,
relocação, hooks e lifecycle de um mesmo módulo não são operações concorrentes.
Os callbacks `log`, `alias`, `relocation_hook` e `initializer_filter` são código
host confiável e não reentrante. Na API 1.3, todas as APIs — inclusive consultas
e `destroy` — detectam uma chamada sobre o mesmo módulo durante callback,
marcam a violação sticky e fazem a operação externa falhar com
`NXLOADER_EREENTRANT` antes de publicar mapping, relocations, resolução,
relatório ou execução de initializer. APIs sobre outro módulo continuam
permitidas. `destroy` é um no-op seguro dentro do callback.

Durante código guest, `INITIALIZING` e `JNI_LOADING` impedem novas mutações e
tornam `destroy` um no-op, mas consultas legítimas ao módulo permanecem
disponíveis. Initializers só entram em `INITIALIZING` depois do preflight
integral; sucesso publica `INITIALIZED`. `JNI_OnLoad` só parte desse estado,
usa lookup literal sem `alias`, recebe `reserved == NULL` e publica `READY`
somente quando a versão retornada aparece exatamente na allowlist. Erro de
símbolo, ABI, tipo, execução ou versão termina em `ERROR`; nada é chamado
implicitamente.

Para múltiplos módulos, carregue e reloque cada dependência em contexto próprio,
registre seus exports com `nxloader_registry_add_module()`, resolva/finalize todos
e execute os inicializadores na ordem nativa: dependências antes do módulo
principal. Os nomes de `DT_NEEDED` e `DT_SONAME` podem ser consultados sem
depender de section headers. O parser de runtime usa exclusivamente program
headers, `PT_DYNAMIC` e SysV/GNU hash; não existe fallback por section.
O módulo provider deve permanecer vivo por mais tempo que o registry e todos os
consumidores resolvidos a partir de seus endereços; destruição não remove nem
invalida exports automaticamente, portanto registry/consumidores devem ser
encerrados antes do provider.
O GNU hash é limitado por um orçamento derivado do tamanho da imagem: os
buckets são lidos uma vez e somente a cadeia do maior bucket é percorrida,
O(buckets + chain). Os nomes `DT_NEEDED` são validados numa cópia privada e a
detecção de duplicatas usa heapsort O(n log n), preservando a ordem original
exposta ao adapter. Todo nome vindo de dynstr — dependência, símbolo, relocation
ou export — tem limite explícito de 4.096 bytes (4.096 + NUL é aceito; 4.097 é
rejeitado), de modo que offsets sobrepostos não recuperem um scan quadrático.

## ABIs e relocações comprovadas

| ABI | Formato | Relocações aceitas por padrão |
|---|---|---|
| AArch64 | ELF64 + RELA | `R_AARCH64_RELATIVE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT` |
| ARMv7 | ELF32 + REL | `R_ARM_RELATIVE`, `ABS32`, `REL32`, `GLOB_DAT`, `JUMP_SLOT` |

Relocações desconhecidas falham antes de qualquer escrita da fase e antes do
hook opcional. O hook só pode decidir sobre tipos que o backend já reconhece;
não existe modo global de “logar e continuar”. TLS/TLSDESC, IFUNC/IRELATIVE,
RELR e Android packed relocations não fazem parte do contrato e são rejeitados
estruturalmente. No ARMv7, os tipos TLS 93, 129 e 130 também são bloqueados antes
do callback, e a presença de qualquer símbolo dinâmico `STT_GNU_IFUNC` invalida
o módulo. Relocações ARMv7 allowlisted também aceitam somente símbolos
`STT_NOTYPE`, `STT_OBJECT` ou `STT_FUNC`; `STT_TLS` e qualquer outro tipo falham
antes do callback.

Nos cinco guests AArch64 aprovados não existe relocation dinâmica `PREL`,
`ADR`, `ADRP`, `ADD`, `LDST`, `JUMP26` ou `CALL26`. Esses tipos permanecem
fail-closed e têm regressões que provam a rejeição antes do callback. Patches de
instrução específicos de Unity, Rockstar ou outra engine pertencem ao adapter e
à API de hook com range, alinhamento e tamanho explícitos; ausência num guest
real nunca é tratada como licença para implementar uma fórmula por suposição.

Cada fase coleta as escritas em O(n), ordena os destinos em O(n log n), valida
overflow, duplicidade e sobreposição e só então faz o commit. Isso mantém a
atomicidade sem a varredura quadrática que seria impraticável nas 305.750
relocações do `libil2cpp` aprovado. Um alvo de relocation também não pode
sobrepor `PT_DYNAMIC`, dynstr, dynsym, SysV/GNU hash ou as próprias tabelas
REL/RELA/PLT; assim uma escrita local não consegue alterar o significado da
fase de resolução seguinte. `DT_INIT_ARRAY` continua relocável, mas nunca é
executado implicitamente.

No AArch64, o destino de uma relocation escalar de 64 bits pode ser
desalinhado, como aceita o linker Android e como ocorre em dados compactados de
IL2CPP reais. O core nunca faz acesso C desalinhado: leitura e commit usam
`memcpy`, e continuam obrigatórios o intervalo completo gravável, a ausência de
sobreposição e a proibição de atingir metadados dinâmicos. A regressão AArch64
usa deliberadamente um slot `R_AARCH64_RELATIVE` em `VMA % 8 == 4`.

Os `PT_LOAD` são obrigatoriamente ordenados e não sobrepostos. Consultas de VMA,
flags, mapping de arquivo e executabilidade usam busca binária; a cobertura GNU
RELRO usa uma única varredura dos intervalos de páginas. Assim milhares de
segmentos hostis não transformam cada relocation ou página em nova varredura
integral da tabela.

`DT_RPATH` e `DT_RUNPATH` também falham fechados: o adapter declara providers e
paths sem permitir que o guest altere a busca do host. Text relocations seguem
negadas por padrão. A única exceção atual é um opt-in ARMv7 estreito,
`NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS`, que ainda exige `DT_TEXTREL` ou
`DF_TEXTREL` no guest e permite somente `R_ARM_CALL`, `R_ARM_JUMP24` e
`R_ARM_THM_CALL` sobre um `PT_LOAD` executável e não gravável. O opt-in não
libera `ABS32`, `REL32` nem outro patch arbitrário em código.
`NXLOADER_CONFIG_FILE_BACKED_TEXT` é o segundo opt-in, também desligado por
padrão, e trata de **residência**, não de permissão. Depois que os segmentos são
colocados, os que são `R+X` (sem `PF_W`) são remapeados direto do arquivo do
guest com `MAP_PRIVATE`, como faz um linker dinâmico. Os bytes são idênticos aos
da cópia — o que muda é a contabilidade: essas páginas viram page cache limpo e
descartável em vez de memória anônima suja, e uma escrita posterior ainda copia
na hora. Medido num port de 345 MiB de `.so`, o segmento `R+X` sozinho responde
por ~315 MiB, então a diferença decide se o jogo cabe num aparelho de 916 MiB.

A elegibilidade é estreita de propósito: só páginas **inteiras**, inteiramente
dentro do arquivo, com VMA e offset congruentes módulo página — exatamente o que
`nxloader_file_mapping_matches()` prova. Bordas parciais, segmentos graváveis e
`.bss` continuam com a cópia. Como os dois caminhos produzem os mesmos bytes, um
remapeamento recusado não muda comportamento nenhum: a função registra e segue,
sem erro. Exige `nxloader_module_load_file()`; com `load_memory` não há arquivo e
a bandeira é inerte. Contrapartida a declarar: página limpa é relida do arquivo
sob demanda, então o arquivo do guest precisa continuar intacto enquanto o
módulo estiver carregado — o mesmo contrato que o linker do sistema sempre teve.

`NXLOADER_CONFIG_FAR_HOOKS` é o terceiro opt-in, também desligado por padrão, e
trata de **alcance de hook**. O pool de trampolins fica no fim da imagem e um `B`
do AArch64 alcança ±128 MiB; num guest grande — o medido aqui tem 330 MiB só de
texto — isso deixa apenas o último trecho do módulo hookável, o que é acidente de
onde o pool calhou de ficar, não um limite deliberado. Com a bandeira ligada o
emissor tenta primeiro o pool do próprio módulo (o caso comum não muda), depois
qualquer pool auxiliar já ao alcance, e só então mapeia uma página nova ao lado
do alvo. Pools auxiliares são `R+X` e só ficam graváveis enquanto um slot é
preenchido; são desmapeados junto com a imagem.

O que **não** muda: sem a bandeira, pool esgotado continua falhando com
`NXLOADER_EBOUNDS` e alvo fora de alcance com `NXLOADER_EOVERFLOW` — os dois são
contratos com teste próprio. E mesmo com a bandeira, se nenhuma página couber ao
alcance o hook é **recusado**: o emissor nunca produz um desvio que não chega. A
regressão confere o `B` emitido, seguindo o deslocamento até o slot e validando
`LDR X17`/`BR X17` e o destino — conferir só o código de retorno deixaria passar
um desvio para o lugar errado.

Metadados `VERSYM/VERNEED` presentes nas referências verdes são tolerados, mas o
registro resolve pelo nome-base do símbolo, como esses loaders comprovados já
fazem; seleção semântica entre duas versões do mesmo nome ainda não é suportada.
Um port que precise desses recursos deve trazer evidência reproduzível e um teste
antes de ampliar o núcleo.

`PT_INTERP` é validado conforme a evidência de cada ABI, não por uma regra cega:
os guests AArch64 aprovados não o possuem e o backend ELF64 falha fechado; o
guest ARMv7 canônico do TASM2 contém `/system/bin/linker`, que é metadata Android
original e permanece aceito. O ELF Linux host/release continua obrigado a usar
o interpretador canônico da ABI e GLIBC no máximo 2.30.

## Imports sem ambiguidade

Imports vêm de providers explícitos: shims do host, módulos auxiliares e exports
do jogo. Prioridade maior vence prioridade menor. Dois endereços diferentes para
o mesmo símbolo, com a mesma prioridade e força, produzem
`NXLOADER_ECOLLISION`; o registro inteiro permanece inalterado. Não há fallback
implícito para `dlsym(RTLD_DEFAULT)`, porque isso fazia libc/GLES do device vencer
um shim sem deixar rastro.

O registro mantém uma ordem bytewise canônica: lookup usa busca binária
`O(log n)` no pior caso, e cada provider entra por um batch transacional ordenado
por heapsort em `O((n+e) log(n+e))`. Assim nomes de exports escolhidos pelo guest
não podem criar o cluster quadrático de uma hash table com linear probing.
Nomes públicos de provider/símbolo seguem o mesmo teto explícito de 4.096 bytes
dos nomes dynstr; erro de limite, memória ou colisão não publica registry nem
relatório parcial.

Uma ordem útil para ports novos é:

- `100`: shims obrigatórios do port;
- `50`: exports de módulos Android auxiliares;
- `0`: funções host deliberadamente expostas pelo adaptador.

Esses números são convenção do port, não valores mágicos do núcleo.

## Hooks opt-in

- `alias`: traduz nomes ao procurar exports ou resolver imports. O caso de nomes
  C++ diferentes entre ARMv7/AArch64 fica no adaptador, não no core.
- `relocation_hook`: pode usar o cálculo padrão, gravar um valor explícito,
  ignorar ou rejeitar uma relocação escalar. `WRITE` é uma resolução explícita;
  `SKIP` só pode deixar o alvo intacto quando o backend ou provider já produziu
  um default válido. Portanto, `SKIP` nunca transforma import strong ausente em
  sucesso nem preserva silenciosamente um `JUMP_SLOT` apontando para PLT0.
  `R_ARM_CALL`, `R_ARM_JUMP24` e
  `R_ARM_THM_CALL` usam somente o codec default validado e não chamam esse
  callback; patch de entrada deliberado usa `nxloader_module_install_hook()`
  depois de `resolve`.
- `initializer_filter`: executa, ignora ou rejeita cada `DT_INIT`/`INIT_ARRAY`.
- `nxloader_module_install_hook()`: patch de entrada somente entre `resolve` e
  `finalize`. AArch64 usa entry `B` de 4 bytes para um trampolim de 16 bytes; ARM
  e Thumb exigem 8 bytes declarados pelo chamador.

O pool é reservado por `trampoline_pool_size`; zero desabilita trampolins e
veneers. No ARMv7, chamadas diretas fora de alcance podem receber um veneer ARM
ou Thumb de 8 bytes no pool, ainda em RW durante a fase transacional e RX após
`finalize`. O pool atual é único e fica depois da imagem: se ele próprio estiver
fora do alcance da instrução, a operação falha sem escrita parcial. Não existem
islands implícitas nem mapeamento RWX. O tamanho sobrescrito dos hooks continua
sempre explícito, evitando corromper a função vizinha.

No AArch64, o hook escreve um `B` alinhado de 4 bytes para um trampolim de 16
bytes (`LDR X17` + `BR X17` + destino de 64 bits). O pool precisa estar dentro
do alcance assinado de ±128 MiB; destino desalinhado, pool insuficiente ou fora
de alcance falham antes de tocar a entry ou consumir capacidade. O pool é RW
somente durante staging e RX depois de `finalize`, com sincronização explícita
do instruction cache.

## ARMv7 softfp

O target opcional `nxloader_softfp` registra os thunks de `libm` usados pelo KOTOR
para a fronteira guest softfp → host hardfp. Ele só pode ser construído por um
toolchain ARM 32-bit:

```sh
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=/caminho/toolchain-armhf.cmake \
  -DNXLOADER_BUILD_SOFTFP=ON
```

Funções GLES que recebem `float` por valor continuam no adaptador gráfico do
port e devem usar `NXLOADER_ARM_SOFTFP`. `difftime` do TASM2 também permanece no
adapter/nxcompat porque mistura PCS e a semântica LP32 do Bionic; nunca deve cair
num `dlsym` hardfp cru.

O módulo nunca é ativado por heurística. O core classifica somente os bits
`EF_ARM_ABI_FLOAT_SOFT` e `EF_ARM_ABI_FLOAT_HARD` em
`nxloader_module_info.arm_float_abi`; bits conflitantes são erro e a ausência de
ambos resulta em `UNSPECIFIED`. Isso preserva o contrato sectionless: evidência
offline de `.ARM.attributes` pode confirmar ou contradizer um pacote, mas não é
uma dependência do parser de runtime. Para guests como o TASM2 que omitem os
bits, o adapter declara explicitamente o PCS observado.

Para exceções C++ ARM EHABI, `nxloader_module_find_arm_exidx()` expõe o
`PT_ARM_EXIDX` do contexto cujo range executável contém o PC. O adaptador pode
usar isso na sua ponte `__gnu_Unwind_Find_exidx` sem recorrer a estado global.

## Build e testes

```sh
cmake -S framework/nxloader -B /tmp/nxloader-build -G Ninja
cmake --build /tmp/nxloader-build
ctest --test-dir /tmp/nxloader-build --output-on-failure
```

ASan + UBSan:

```sh
cmake -S framework/nxloader -B /tmp/nxloader-asan -G Ninja \
  -DNXLOADER_ENABLE_SANITIZERS=ON
cmake --build /tmp/nxloader-asan
ctest --test-dir /tmp/nxloader-asan --output-on-failure
```

O gate host completo executa GCC, Clang com ASan/UBSan, análise estática e
20.000 casos libFuzzer, sempre numa árvore `mktemp` própria. Os testes de
lifecycle percorrem o preflight com todos os initializers filtrados como
`SKIP`; nenhum código initializer ou JNI guest é executado:

```sh
bash framework/nxloader/tests/run-host.sh
python3 -B framework/nxloader/tests/test_m11_audit.py
```

O gate ARMHF cruzado constrói a mesma base com GCC e Clang/LLD sobre o sysroot
público de baixa glibc, audita todos os ELFs gerados e roda em QEMU apenas código
host de teste. Ele chama thunks softfp reais de `float` e `double`, mede stack de
8 bytes na entrada pública e verifica a preservação de `d8`; nenhum ELF guest ou
initializer é carregado:

```sh
bash framework/nxloader/tests/run-armv7-cross.sh
```

O gate AArch64 cruzado usa uma imagem Debian Buster fixada por digest, sem rede e
com a fonte montada read-only. GCC 8.3 e Clang/LLD constroem contra o mesmo
sysroot; QEMU executa somente probes próprios de LP64/AAPCS64, argumentos
inteiros e FP, SP de 16 bytes, preservação de `x19–x28` e `d8–d15`, e uma
reescrita RW→RX com cache sync. Um ELF sectionless criado integralmente pelo
teste mantém um único módulo e uma única mapping owner. Ele percorre
`load → relocate → resolve → finalize`, executa entry/pool A, reabre exatamente
os mesmos VAs com `nxloader_module_begin_patch()`, reescreve entry e veneer
pelo `install_hook`, chama outro `finalize` real e executa entry/pool B. A entry de
oito bytes começa nos quatro bytes finais de uma página medida, cruzando também
a borda de cache line. As identidades de mapping, entry e pool são conferidas
antes e depois da reabertura. Somente cada `finalize` torna o código RX e
faz cache sync; o fluxo positivo não usa `mprotect` nem clear manual do harness.
Nenhum initializer/JNI ou guest externo é carregado.
O runner audita `PT_INTERP`, `DT_NEEDED`, stack, W^X e GLIBC de todo ELF
temporário:

```sh
bash framework/nxloader/tests/run-aarch64-cross.sh
```

O gate opcional dos guests AArch64 exige que os cinco caminhos e o inspector
sejam fornecidos explicitamente. Cada arquivo é preso por SHA-256 e aberto
read-only; para cada guest executa exatamente `--relocate` e `--exports`. O
segundo modo repete a relocação, cria um registry e chama `add_module` uma vez,
comparando as contagens com um inventário ELF estático independente. São dez
subprocessos guest no total, todos sem resolver imports, finalizar, chamar
`DT_INIT`, `INIT_ARRAY` ou `JNI_OnLoad`, copiar dados ou acessar device. Sem todos
os inputs, retorna `SKIP` de forma explícita. Cada subprocesso tem deadline
monotônico de 120 s, limite de 256 KiB por stream e sinalização presa ao PID por
`pidfd`; TERM seguido de KILL é usado somente no filho exato. A evidência inclui
um snapshot ordenado dos 11 fontes usados no inspector, além dos hashes do
binário e dos guests. Nove self-tests do supervisor cobrem inclusive parsing do
relatório, contagem de exports e isolamento do subprocesso `--exports`:

```sh
python3 -B framework/nxloader/tests/run_m10_aarch64_guest_gate.py \
  --bully2 /caminho/libGame.so \
  --sonic4 /caminho/libfox.so \
  --horizon-unity /caminho/libunity.so \
  --horizon-il2cpp /caminho/libil2cpp.so \
  --horizon-main /caminho/libmain.so \
  --inspector /caminho/nxloader_inspect \
  --output-dir /caminho/evidencia
```

O probe ARMv7 `tests/test_armv7_cache_sync.c` permanece manual. A atestação
AArch64 M124 não mantém uma segunda implementação solta: ela estende
`tests/test_aarch64_cross.c`, carrega apenas o ELF sintético anônimo do teste e
exercita entry e veneer pelo caminho real do loader. Depois do patch, somente
`nxloader_module_finalize()` faz cache sync e fecha as páginas em RX.

O runner preserva a distinção de evidência. A execução canônica abaixo produz
`EMULATED`, `hardware_ran=false` e `physical_pending=true`; compilar sem
executar nunca produz PASS. Os dois ELFs auditados, o receipt e o log integral
hashado ficam no diretório novo indicado, fora do source tree:

```sh
python3 -B tests/run_aarch64_icache_attestation.py run \
  --output-dir /caminho/novo/m124-evidence
python3 -B tests/run_aarch64_icache_attestation.py compile-only \
  --output-dir /caminho/novo/m124-compiled-evidence
python3 -B tests/run_aarch64_icache_attestation.py validate \
  --receipt /caminho/novo/m124-evidence/m124-aarch64-icache-receipt-v1.json
```

`compile-only` constrói e audita os dois artefatos, não executa binário target e
emite somente `COMPILED_ONLY/PENDING`, `executed=false` e `hardware_ran=false`.
Não existe comando owner-local capaz de emitir `PHYSICAL`: essa classe permanece
`PENDING` e não promovível até existir trust anchor externo especificado e
implementado. Arquitetura do host ou JSON fornecido pelo chamador nunca promovem
evidência. Receipt stale, truncado, duplicado, com cenário ausente, hash
divergente, classe forjada ou path privado é recusado.

O gate canônico atual exige Docker e a imagem local
`playfetch-builder:buster` presa ao ID declarado no script: dela vêm GCC 8.3 e o
sysroot de baixa glibc usados pelos dois builds. Um QEMU/sysroot do host pode ser
útil em desenvolvimento dirigido, mas não substitui essa fronteira da bateria.
A M124 acrescenta a fronteira runtime `begin_patch` sem renumerar nenhum valor,
campo ou assinatura 0.8.0. Por isso o componente passa a `0.9.0`, preserva API
`1.3` e mantém a closure 0.8.0 imutável ao lado da closure aditiva 0.9.0.

Os testes constroem ELFs sintéticos válidos e corrompidos para os dois ABIs,
verificam bounds, relocação atômica, weak imports, alias, colisão/prioridade,
hooks, permissões, BSS zero-fill, `DT_NEEDED`, `DT_SONAME`, ownership e cleanup.
No ARMv7 também cobrem addends REL positivos/negativos, REL32 modular,
CALL/JUMP24/THM_CALL, interworking, alcance, veneers, weak fallthrough, opcodes
hostis e capacidade do pool.
No AArch64 cobrem RELA signed, overflow/underflow, alvos desalinhados ou em
código/metadados, destinos duplicados, IFUNC/IRELATIVE/TLS/unknown antes do
callback, bytes exatos do trampolim, alcance de ±128 MiB e capacidade atômica.
Os dois parsers também exercitam 32 buckets GNU compartilhando uma cadeia de
oito entradas, e 2.048 `DT_NEEDED` únicos/invertidos mais duplicata hostil, para
impedir a reintrodução dos caminhos quadráticos. Há ainda 512 offsets dynstr
sobrepostos nos limites 4.096/4.097, 4.096 `PT_LOAD` com 4.096 relocações e um
gap RELRO adversarial, nos dois ABIs. O registry recebe ainda 2.048 nomes
distintos que colidem nos mesmos 12 bits baixos de FNV-1a, validando lookup
binário, ingestão batch, duplicatas ordenadas e rollback integral.
Uma varredura determinística adicional exercita 2.048 mutações antes do fuzz.
Os gates cross executam somente probes próprios e nunca carregam ou inicializam
um guest.

As regressões M11 exercitam os quatro callbacks host, incluindo `log`, com uma
tentativa de cada API pública sobre o próprio módulo e uma consulta válida a
outro módulo. ELF32 e ELF64 comprovam rollback de relocation/resolução,
relatório intacto e `destroy` no-op. Fixtures com `DT_INIT` + `INIT_ARRAY`
comprovam preflight completo, ordem, filtro tardio e exactly-once sem executar
código guest. A API JNI é testada apenas no caminho de export ausente: valida
JavaVM/reserved/flags/limite/duplicatas da allowlist, ausência obrigatória versus
OPTIONAL, lookup sem alias e publicação de `READY`; nenhuma função JNI real é
chamada pelo gate host.

Para auditar um `.so` sem executar código guest:

```sh
/tmp/nxloader-build/nxloader_inspect libGame.so --relocate
```

Use `--exports` para também registrar todos os exports do guest e validar
colisões/escala do índice sem resolver imports ou executar o módulo.

O relatório de referências reais fica em [REFERENCE-AUDIT.md](REFERENCE-AUDIT.md).

## Integração mínima

```c
nxloader_config cfg;
nxloader_module *game = NULL;
nxloader_registry *imports = NULL;

nxloader_config_init(&cfg);
cfg.expected_arch = NXLOADER_ARCH_AARCH64; /* ou ARMV7 no binário ARMHF */

nxloader_module_create(&cfg, &game);
nxloader_registry_create(&imports);
/* nxloader_registry_add_provider(imports, &shims_do_port, NULL); */
nxloader_module_load_file(game, "libGame.so");
nxloader_module_relocate(game);
nxloader_module_resolve(game, imports, 0, NULL);
/* aliases e hooks comprovados já foram declarados pelo adaptador */
nxloader_module_finalize(game);
nxloader_module_call_initializers(game);
/* Se e somente se o fluxo comprovado tiver JNI_OnLoad:
 * nxloader_jni_onload_options jni = {
 *   sizeof(jni), java_vm_real, NULL, versoes_aceitas,
 *   quantidade_de_versoes, 0
 * };
 * nxloader_module_call_jni_onload(game, &jni, &versao_retornada);
 */
/* Agora o adaptador continua o fluxo Android real; nada é inventado. */
```

Em código de produção, cheque cada retorno e interrompa o boot com uma mensagem
útil ao usuário. `NXLOADER_RESOLVE_ALLOW_UNRESOLVED` é apenas para bring-up
controlado e não deve entrar numa release. Com esse opt-in, imports strong ainda
aparecem em `unresolved_strong` e seus slots permanecem intactos deliberadamente;
sem ele, qualquer strong ausente aborta o commit inteiro e o módulo continua em
`RELOCATED`, permitindo retry com providers completos.

`nxloader_module_destroy()` apenas libera o mapeamento e o estado host. Durante
callback host, `INITIALIZING` ou `JNI_LOADING`, ele é um no-op defensivo. Ele não
inventa nem chama teardown guest, `DT_FINI`, `JNI_OnUnload` ou callbacks da
engine. O adaptador deve percorrer o shutdown/persistência nativos comprovados
antes de destruir o contexto (ou aplicar o deadline seguro definido pelo port).

## Release e glibc

`libnxloader.a` não possui requisito de glibc próprio: seus objetos são ligados
ao loader do jogo. Todo ELF Linux resultante de uma release pública deve exigir
no máximo `GLIBC_2.30`, idealmente `GLIBC_2.17`. Use o sysroot público de baixa
glibc e audite o loader final e todas as bibliotecas Linux construídas pelo
projeto. `tools/check-glibc.sh` é um gate adicional; ele não substitui o gate do
pacote completo.

O código não seleciona Mesa, GLES 3.2, backend SDL, placa de áudio ou device. A
negociação de plataforma pertence ao `nxcompat`; o bootstrap PortMaster e o
NXExtract acontecem antes do processo e também ficam fora deste núcleo.

## Proveniência

O desenho foi derivado das soluções validadas nos ports Bully2 e Sonic 4 Episode
II AArch64, cruzado com o conjunto multi-módulo grande do Horizon Chase e com
KOTOR/TASM2, Stardew e GTA aprovados. Apenas contratos de ABI, ordem, W^X e
logging foram generalizados; aliases, offsets, filtros de constructor, JNI e
hooks de cada engine permanecem no adapter. A linhagem original de `so_util`
(`max_arm64`, mtojek e demais créditos) permanece coberta pelo `NOTICE` do
repositório. Este módulo é distribuído sob a licença do projeto.
