# M11 — auditoria Bionic, NDK, JNI e lifecycle

Data da auditoria: 2026-08-08. O escopo é exclusivamente estático e somente
leitura. Nenhum ELF guest foi mapeado, resolvido ou executado; nenhum
constructor, `JNI_OnLoad`, método JNI ou lifecycle foi chamado; nenhum device ou
rede foi acessado. Os ports usados como referência ficaram intactos.

O inventário normativo é `m11-android-guests-v1.json`. Ele contém cada nome UND
dinâmico, inclusive os que não pertencem a Bionic/NDK/JNI, os bindings weak, os
`DT_NEEDED`, exports `JNI_OnLoad`/`Java_*`, `DT_INIT`, `DT_INIT_ARRAY`,
`PT_INTERP`, hashes e as sequências de lifecycle presas às linhas das fontes.

## Fontes positivas e identidade

Somente referências marcadas positivas no catálogo foram usadas:

- Bully2: `ports/bully2/libGame.so`, SHA-256
  `345f6411bebbd2310b2cb5d2a271341847a054c2b7f6d5ffba8030cd53140d76`;
- Sonic 4 Episode II v3/AArch64: `ports/sonic4/device_libs_3.0.0_arm64/libfox.so`,
  SHA-256 `ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a`;
- Horizon Chase: `libunity.so`, `libil2cpp.so` e `libmain.so`, respectivamente
  `7c46972d06b6b85f953bf61b477fdbf5bf22f7be55b57e4b54b4403f05478359`,
  `acf414c77853aa0e7f8d8963f307f65f3d156c8d6d806661b738df3385e48cdc` e
  `a2dfe24e0170af0ae90b13630fc6d4e788be2c5216b4bddb17a6e0ca3c4c5af8`;
- KOTOR: o catálogo fixa `ports/kotor/libKOTOR.so` em
  `099c43db23a891a19f40f5805c3582b390de39d96d6e50be52649a40498a68b5`.
  O inventário M11 fixa também as seis dependências que o adapter aprovado
  carrega explicitamente em `ports/kotor/src/main.c:266-303`;
- TASM2: exclusivamente `ports/asm2_127`. `libtasm2.so` tem SHA-256
  `d091fe95c56af681f1a06453e9868622935ecbb759c05c340fc16bf8df2ae62e` e
  `libgenerator.so` tem
  `3871b9c4ae5d1c491abbccc7af1744121bf964f52b4812d2f672c72ff7c062ed`.

`ports/tasm2` é negative-only e proibido tanto pelo catálogo quanto pelo
reprodutor. Nenhum arquivo dessa árvore foi lido ou usado.

## Resultado ELF estático

As contagens UND são únicas dentro de cada módulo. As quatro categorias são
exaustivas, porém servem apenas para auditoria: não autorizam fallback nem
alteram o binding ELF. `JNI UND` fica zero na maioria dos módulos porque JNI é
chamado pela vtable `JNIEnv`/`JavaVM`, e não por funções ELF importadas.

| Módulo | UND | Bionic | NDK | JNI UND | Outros | Weak-only | `JNI_OnLoad` | `Java_*` | `INIT_ARRAY` |
|---|---:|---:|---:|---:|---:|---:|:---:|---:|---:|
| Bully2 `libGame` | 417 | 147 | 13 | 0 | 257 | 2 | sim | 38 | 506 |
| Sonic `libfox` | 374 | 296 | 13 | 0 | 65 | 1 | sim | 98 | 46 |
| Horizon `libunity` | 423 | 299 | 85 | 0 | 39 | 28 | sim | 3 | 433 |
| Horizon `libil2cpp` | 288 | 286 | 2 | 0 | 0 | 0 | sim | 0 | 24 |
| Horizon `libmain` | 11 | 11 | 0 | 0 | 0 | 0 | sim | 0 | 0 |
| KOTOR `libLzmaLib` | 15 | 14 | 0 | 0 | 1 | 0 | não | 0 | 0 |
| KOTOR `libminiz` | 30 | 29 | 0 | 0 | 1 | 0 | não | 0 | 0 |
| KOTOR `libfreetype` | 38 | 36 | 0 | 0 | 2 | 0 | não | 0 | 0 |
| KOTOR `libfmod` | 120 | 111 | 1 | 0 | 8 | 4 | sim | 2 | 3 |
| KOTOR `libhidapi` | 43 | 41 | 1 | 0 | 1 | 0 | não | 8 | 1 |
| KOTOR `libandroid_port` | 403 | 143 | 12 | 2 | 246 | 1 | não | 0 | 3 |
| KOTOR `libKOTOR` | 487 | 141 | 112 | 2 | 232 | 4 | não | 14 | 28 |
| ASM2 `libgenerator` | 83 | 56 | 0 | 0 | 27 | 1 | não | 2 | 7 |
| ASM2 `libtasm2` | 396 | 204 | 15 | 0 | 177 | 1 | sim | 92 | 223 |

São 14 módulos, 3.128 nomes UND módulo-locais e 1.274 slots de
`DT_INIT_ARRAY`. Todos os 14 têm `DT_INIT` ausente. Isso prova presença e
tamanho das tabelas, não autoriza executá-las automaticamente: o gate sintético
do componente cobre a mecânica e cada adapter decide a ordem guest.

Dois guests possuem `PT_INTERP=/system/bin/linker`: KOTOR `libfmod.so` e ASM2
`libtasm2.so`. É metadado do ELF Android original, nunca o contrato do host
Linux. Os demais 12 módulos não têm `PT_INTERP`.

Os únicos imports JNI explícitos são `Android_JNI_GetEnv` e
`SDL_AndroidGetExternalStoragePath` em `libandroid_port` e `libKOTOR`. O adapter
fornece os globals internos do SDL/Aspyr em `ports/kotor/src/imports.c:931-940`.
Todos os demais detalhes JNI estão nas vtables e nos exports enumerados no JSON.

## Contratos comuns que os guests provam

1. **ABI antes do fallback — crítico.** `FILE`, `__sF`, `stat/statfs`,
   `dirent`, `sigaction/sigset`, mutex/cond/rwlock e semáforos Android não podem
   ser passados diretamente à glibc. O KOTOR prova `stat` ARM32 de 104 bytes e
   `dirent.d_name` em `+19` (`ports/kotor/src/libc_shim.c:271-348`); o ASM2 fixa
   os mesmos layouts em `ports/asm2_127/src/bionic_compat.c:124-149`; Bully2
   prova a divergência pthread LP64 em `ports/bully2/src/pthread_bridge.c:1-14`.

2. **Resolução exata — crítico.** Um UND strong precisa de provider/adaptador
   exato ou a carga falha antes de código guest. A tabela gerada do Horizon que
   transforma imports desconhecidos em zero (`ports/horizonchase/src/imports.gen.c:1-8`)
   é evidência negativa, não uma base universal. O adapter precisa substituir
   explicitamente Bionic/NDK antes do resolve e corrigir os slots necessários,
   como em `ports/horizonchase/src/main.c:5915-6085`.

3. **Ordem nativa — crítico.** Para cada módulo realmente carregado: mapear,
   relocar, instalar adaptações ABI, resolver, finalizar/limpar cache, executar
   constructors aceitos, estabelecer `JavaVM`/`JNIEnv` estáveis e só então
   chamar o `JNI_OnLoad` correspondente. Os entry points de Activity/surface e
   o loop vêm depois. KOTOR demonstra a forma por módulo em
   `ports/kotor/src/main.c:110-159`; ASM2 demonstra o gate explícito antes dos
   constructors e valida JNI 1.4 em `ports/asm2_127/src/main.c:430-579`.

4. **Identidade JNI — crítico.** `AttachCurrentThread`, `GetEnv`, referências,
   classes, IDs, strings, arrays, exceptions e `GetJavaVM` precisam manter
   identidade e semântica. A base mais completa é
   `ports/asm2_127/src/jni_bridge.c:1032-1075,2022-2318`.

5. **`RegisterNatives` real quando consumido — crítico condicional.** Horizon
   registra e depois busca `initJni`, surface, focus e render
   (`ports/horizonchase/src/jni_shim.c:2569-2598` e
   `ports/horizonchase/src/main.c:6544-6587`). ASM2 guarda por classe, nome e
   assinatura e suporta unregister/lookup
   (`ports/asm2_127/src/jni_bridge.c:1806-1915`). Retornar sucesso sem guardar
   ponteiros só é aceitável na exceção KOTOR hash-pinned, cuja entrada atual é
   por exports `Java_*` e `SDL_main`.

6. **Lifecycle pertence ao adapter — crítico.** O core pode validar fases, mas
   classes Java, exports, callbacks, offsets, caminhos, surface e teardown são
   específicos de Rockstar, fox/f2f, Unity, Aspyr ou Gameloft. Nada disso deve
   entrar no código universal.

## Sequências aprovadas, sem generalizar a engine

### Bully2

O loader carrega primeiro `libc++`, captura seus exports e só depois carrega
`libGame`; cada módulo já passou por constructors antes de `jni_load`
(`ports/bully2/src/main.c:754-816`). O adapter cria VM/env, chama
`JNI_OnLoad`, `implOnInitialSetup`, `implOnActivityCreated`, cria GL antes da
surface, chama `implOnSurfaceCreated`, `implOnSurfaceChanged`, resume e entra em
`implOnDrawFrame` (`ports/bully2/src/jni_shim.c:5532-5760`).

Rockstar exports, callbacks de login, offsets relativos a `StorageRootPath` e
globals EGL são adapter-specific. Riscos não promovidos: `JNI_OnLoad` ausente ou
com versão inválida não falha; `RegisterNatives` guarda apenas o último ponteiro;
slots desconhecidos retornam zero; não há pause/stop/destroy comprovado antes do
`_exit` de workers process-lifetime (`ports/bully2/src/main.c:815-822`).

### Sonic 4 Episode II

Depois do resolve/finalize, o adapter valida BuildID, tamanho e contagens de
instruções antes do patch exato de stack guard; só então libera cache e executa
constructors (`ports/sonic4/src/main.c:551-600,1225-1244`). Esse patch nunca é
universal. A sequência continua com VM/env, `JNI_OnLoad`, contexto corrente,
exports fox, `SetGamePath` LPK/root antes de `init`, idioma, f2f, dimensões reais,
`init`, `DrawEGLCreated`, `resumeEvent` e thread `FileProcess`
(`ports/sonic4/src/main.c:1714-1886`). O loop chama `GameProcess` antes de
`DrawFrame` (`ports/sonic4/src/main.c:2337-2373`).

Paths, callbacks de intro/anúncio, áudio fox e os 98 exports Java são
adapter-specific. Riscos não promovidos: `JNI_OnLoad` fail-open, slots JNI
desconhecidos em zero e saída por sync/`_exit` sem pause/destroy demonstrado
(`ports/sonic4/src/main.c:424-438`).

### Horizon Chase

O fluxo aprovado carrega somente `libunity` e `libil2cpp`: Unity recebe seus
adapters, finalize, constructors e `JNI_OnLoad`; IL2CPP é carregado depois e usa
a mesma VM, também com finalize, constructors e `JNI_OnLoad`
(`ports/horizonchase/src/main.c:5905-6085,6390-6527`). O adapter volta ao módulo
Unity, consome `RegisterNatives`, cria o backend gráfico, chama `initJni`,
surface-created/recreate, surface-changed/event, resume e focus true
(`ports/horizonchase/src/main.c:6544-6653`). HandlerThread/Choreographer precedem
o render (`ports/horizonchase/src/main.c:6694-6717`). Na saída: focus false,
pause, flush de preferências, parada/join do áudio e `_exit`; `nativeDone` é
somente diagnóstico porque espera o handshake Java ausente
(`ports/horizonchase/src/main.c:7057-7075`).

`libmain.so` exporta `JNI_OnLoad`, mas o adapter não o mapeia e trata
`libmain` como o próprio processo no `dlopen` (`ports/horizonchase/src/main.c:4897-4908`).
Logo seu hash, UND e export são prova estática; chamar seu `JNI_OnLoad` não é
parte do lifecycle aprovado. PlayerPrefs, FMOD, class loader, Handler,
Choreographer, Google callbacks e surface choreography são Unity/Horizon-only.

### KOTOR

O adapter inicializa JNI e carrega, leaves-first, `libLzmaLib`, `libminiz`,
`libfreetype`, `libfmod`, `libhidapi` opcional, `libandroid_port` e `libKOTOR`
(`ports/kotor/src/main.c:256-303`). Cada módulo passa por constructors; se ele
exporta `JNI_OnLoad`, a chamada é imediata e retorno `<=0` falha. Nesse conjunto,
somente `libfmod` exporta `JNI_OnLoad`. `libKOTOR` exporta 14 `Java_*`, mas não
`JNI_OnLoad`.

Depois o adapter resolve os exports Aspyr, chama `nativeCreateMutex`,
`nativeOnResume`, monta os OBBs e entra em `SDL_main`
(`ports/kotor/src/main.c:307-358`). `SDL_main` é dono de janela, GL, eventos,
áudio e terminal; não há uma sequência separada de pause/destroy comprovada no
wrapper. `libhidapi` é o único módulo inteiro opcional, porque o SDL nativo pode
gerir os controles.

Os layouts ARM32 e thunks softfp são comuns como disciplina, mas OBB/Aspyr,
`SDL_main`, FMOD e a ordem dos sete módulos são adapter-specific. As respostas
JNI genéricas de conectado/disponível em `ports/kotor/src/jni_fake.c:82-108`
são política do port e não podem virar default universal.

### TASM2 1.2.7d canônico

O adapter inicializa representações Bionic, mapeia/reloca `libtasm2`, aplica a
tabela de diferenças antes do fallback, resolve/finaliza e exige `ASM2_RUN`
antes de qualquer constructor (`ports/asm2_127/src/main.c:401-479`). Depois
resolve exports, monta a VM/env completa, chama `JNI_OnLoad` e exige exatamente
JNI 1.4 (`ports/asm2_127/src/main.c:481-579`). A sequência Gameloft continua com
Activity/social/platform/device/billing/notification/data-sharing/GameOptions,
GL/view/paths, state true, resize, primeiro frame antes do listener, loop e
pause/exit exatamente uma vez (`ports/asm2_127/src/main.c:581-916`).

`libgenerator.so` possui 7 constructors e 2 exports Java, mas não é carregado
pelo `main.c` atual. É evidência estática do pacote canônico; não existe prova
para chamar seus constructors ou exports no adapter atual.

## Stubs permitidos e sua semântica exata

Não existe stub universal. O JSON fixa todas as exceções por port, hash,
precondição, retorno e efeito. O resumo é:

- Bully2: apenas três getters TLS exatos são `void` sem efeito; se OpenAL foi
  realmente detectado ausente, o fallback opcional oferece handles/IDs válidos,
  reporta source parado e deixa o áudio explicitamente mudo
  (`ports/bully2/src/imports.c:3122-3207`).
- Sonic: o token ALooper é estável, acquire/release/remove/wake são inertes,
  device/button state é zero, `eglReleaseThread` retorna sucesso e `pollOnce`
  delega a `pollAll` (`ports/sonic4/src/imports.c:252-262,332-332,795-825`).
- Horizon: somente a lista explícita de sensores/looper/profiler não consumida
  pelo path gráfico retorna zero/NULL
  (`ports/horizonchase/src/main.c:5124-5125,6086-6097`). Acquire/release da
  janela única são no-op apenas porque o adapter possui o token por todo o
  processo (`ports/horizonchase/src/main.c:1295-1306`).
- KOTOR: `RegisterNatives` retorna `JNI_OK` sem registry somente para os sete
  módulos pinados e o atual fluxo de exports estáticos/SDL. Nomes de thread,
  scheduling, atfork e thread-atexit são hints process-lifetime sem efeito
  (`ports/kotor/src/jni_fake.c:207-218`; `ports/kotor/src/libc_shim.c:59-66`).
- ASM2: o bridge de sensor oferece tokens estáveis, nenhum sensor default, zero
  eventos e preserva os retornos distintos de enable/rate com sensor NULL
  (`ports/asm2_127/src/platform_shims.c:224-279`). Os métodos Java de vídeo,
  teclado, browser, tracking e toast têm retornos vazios exatos
  (`ports/asm2_127/src/android_callbacks.c:1578-1600`). Só a versão 1.2.7d pode
  omitir `GL2JNILib_destroy`, e somente depois de pause/options/input/exit
  (`ports/asm2_127/src/main.c:865-916`).

Ficam proibidos: `jni_stub` genérico como prova, `stack_chk_fail` que retorna,
`JNI_OnLoad` ausente tratado como sucesso, generated-import zero stub, lifecycle
pulada, window/asset dummy sem ownership, estado online/consent inventado e
qualquer weak/no-op inferido de um nome desconhecido.

## Reprodução determinística

O inventário automático pertence ao gate de processo selado, porque seu
supervisor pode encerrar exclusivamente o subprocesso `readelf` em timeout ou
limite de saída. Execute somente pela entrada canônica:

```sh
bash framework/nxbootstrap/tests/run-isolated.sh
```

O script Python é uma implementação interna desse gate e não é uma entrada de
host autorizada. A matriz o classifica como arquivo de suporte do gate
`bootstrap-isolated`, nunca como gate `filesystem`.

O reprodutor usa paths relativos, rejeita symlinks e a árvore legacy, valida
schema/IDs/ABIs/hashes, rejeita chaves JSON duplicadas, remove variáveis de
loader do ambiente do `readelf`, limita tempo e saída por pipes e compara todo o
resultado com o JSON versionado. A saída verde termina com
`guest_code_executed=0`, `initializers_executed=0`,
`jni_onload_executed=0`, `device_access=0` e `network_access=0`.

`readelf` apenas interpreta bytes dos arquivos pinados. A ferramenta não chama
`nxloader`, `dlopen`, `mmap` do guest, resolver, finalize, initializer ou JNI.
Nenhum guest é copiado para `framework` ou incluído em release.
