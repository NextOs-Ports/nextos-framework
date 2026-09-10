# Shims: do import ao contrato testado

[English](../en/SHIMS.md)

Um shim adapta uma interface esperada pelo guest Android ao host Linux. A implementação correta preserva assinatura, convenção de chamada, layout, ownership, duração, erros e callbacks. Encontrar o nome do símbolo não demonstra compatibilidade.

## 1. Estudar o exemplo executável

Comece em [examples/shims-reference](../../examples/shims-reference/README.md). `shims.h` define assinaturas e uma união de ponteiros de função; `shims.c` registra somente implementações explícitas; `main.c` exercita seus contratos.

| Contrato | Comportamento demonstrado | Limite |
| --- | --- | --- |
| `__errno` | Retorna o endereço de `errno` da thread chamadora | Não cobre todo o TLS Bionic |
| `__android_log_write` | Diagnóstico em stderr; 1 ao entregar, erro negativo ao falhar | Não implementa filtragem/logd Android |
| `nx_demo_property` | Somente `demo.name`, com capacidade explícita | Não é `__system_property_get` |
| Resolver | Nome ausente ou assinatura enumerada errada retorna NULL | A enumeração não infere o ABI de um ELF |

O resolver didático não faz relocações. Antes de integrar ao loader real, a IA precisa identificar o tipo do símbolo ELF e a assinatura pelo contrato da biblioteca/engine. Um objeto como `__stack_chk_guard` não pode virar endereço de função.

## 2. Inventariar os imports

Com uma biblioteca do dono preparada em área privada, use `readelf -Ws` para a tabela de símbolos, `readelf -d` para dependências e `readelf -r` para relocações. Separe símbolos indefinidos obrigatórios, fracos, objetos e TLS. Verifique imports obtidos por `dlsym`, `eglGetProcAddress` e JNI em runtime: a lista estática não os contém necessariamente.

Uma tabela de trabalho útil tem: nome; versão do símbolo; tipo; ABI; assinatura; chamador; resultado/erro esperado; dono do buffer; thread; implementação escolhida; fonte/hash/licença; testes; limite. Essa tabela pertence ao novo port.

## 3. Decidir o tipo de adaptação

| Caso | Decisão |
| --- | --- |
| ABI e semântica comprovadamente idênticos | Encaminhar para o host com tipo correto |
| Layout/enum/erro diferente | Traduzir explicitamente entrada e saída |
| Callback assíncrono ou objeto com estado | Implementar ciclo de vida e sincronização |
| Serviço opcional ausente | Retornar a ausência prevista pelo contrato, com prova do caminho |
| Serviço obrigatório desconhecido | Falhar com diagnóstico preciso e implementar antes de aceitar |

Não converter indiscriminadamente `pthread_mutex_t`, `FILE`, estruturas de sinal, `dirent` ou objetos JNI. Não forçar todo mutex a recursivo. Ponteiros gerenciados, handles JNI e ponteiros nativos não são intercambiáveis.

## 4. Implementar um contrato novo

Escreva primeiro os casos que distinguem uma implementação correta de um stub: input válido, limite de buffer, erro real, NULL quando permitido, chamada concorrente, ownership e destruição. Depois registre a função tipada e integre um chamador conhecido. Não use casts genéricos para silenciar incompatibilidade.

Em JNI, registre classes, métodos e campos por assinatura exata. Preserve referências locais/globais, exceções, conversão de strings e associação da thread. `JNIEnv` pertence à thread; não compartilhe um ponteiro arbitrário entre workers. [Orientações oficiais de JNI](https://developer.android.com/ndk/guides/jni-tips).

## 5. Localizar implementações maiores

| Fronteira | Fonte inicial |
| --- | --- |
| ELF e relocações | [nxloader](../../framework/nxloader/README.md) |
| Android/lifecycle | [nxandroid](../../framework/nxandroid/README.md) |
| Contexto/present | [nxgl](../../framework/nxgl/README.md) |
| Mixer/saída | [nxaudio](../../framework/nxaudio/README.md) |
| Controle e contextos | [nxinput](../../framework/nxinput/README.md) |
| Unity | [Casos públicos e diagnóstico](../../portando_unity/README.md) |
| Mono/.NET, Godot, Cocos | [Mono](MONO-ANDROID.md), [Godot](GODOT.md), [Cocos2d-x](COCOS2D-X.md) |

Leia os manifestos e as licenças antes de adaptar código. O código upstream pode conter soluções específicas que não devem virar defaults do framework.

## 6. Medir a cobertura honestamente

Classifique cada contrato como `implemented-tested`, `adapter-required`, `optional-absent` ou `unsupported`. Registre o perfil de dados/ABI em que foi exercitado. “Todos os imports resolvidos” mede resolução; não mede gameplay, concorrência, save ou fidelidade gráfica.

A base ampla será construída por famílias e contratos repetidos no catálogo. O exemplo atual ensina a estrutura; não é um shim Android quase completo nem uma promessa de rodar a maioria de todos os jogos. Toda mudança compartilhada da V5 exige desenvolvimento separado na linha V6.
