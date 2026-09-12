# Android nativo C/C++: JNI, NativeActivity e SDL

[English](../en/NATIVE-ANDROID.md)

Esta trilha atende bibliotecas nativas de jogos **Android**, inclusive engines próprias, cujo adapter executará em Linux ARM. Use a [seleção de runtimes](ANDROID-RUNTIMES.md) para identificar antes uma trilha específica. A ausência de Unity, Godot ou Mono não comprova que o restante do jogo dispense Java.

## 1. Inventariar bibliotecas e bootstrap

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/native-inventory.json
python3 tools/find_reference.py --runtime native-android --abi arm64-v8a
python3 tools/find_reference.py --runtime native-android --abi armeabi-v7a
```

Registre package/versão, ABIs do guest, ELFs e hashes, `DT_NEEDED`, imports, relocações, TLS, construtores, classes Java e serviços Android usados. O [inventário](INVENTARIO-ANDROID.md) não executa DEX nem descobre todas as chamadas via JNI/dlsym. Leia a Activity e seus callbacks na cópia local autorizada para completar o mapa.

Prefira AArch64 quando disponível; wrapper Linux 64 bits não transforma guest ARMv7/x86 em AArch64. Engine desconhecida continua `unknown` no catálogo mesmo quando a família nativa está demonstrada. Engines híbridas, como Off The Road com xGen/Horde3D+bgfx sobre uma casca Cocos2d-x, exigem separar a inicialização do renderer.

## 2. Escolher uma referência pelo contrato

Todas as referências abaixo já são fontes públicas selecionadas nesta coleção. Abra manifesto, README e licença antes de reutilizar; pins históricos não são uma migração V5.

| Fonte | Commit selecionado | Fronteira para estudar |
| --- | --- | --- |
| [Action Squad](../../ports/actionsquad-nextos/README.md) · [SOURCE-MAP](../../ports/actionsquad-nextos/SOURCE-MAP.json) | `842efc61a47192cd3ac4d3062dbe9c32e808c791` | AArch64, NativeActivity e fila de input |
| [KOTOR](../../ports/kotor-nextos/README.md) · [SOURCE-MAP](../../ports/kotor-nextos/SOURCE-MAP.json) | `9d8dc0cdd3cf164a39f480200ab20a9a1117b91b` | ARMv7, Odyssey/SDL2, OBB e contexto da engine |
| [Angry Birds](../../ports/angrybirds-nextos/README.md) · [SOURCE-MAP](../../ports/angrybirds-nextos/SOURCE-MAP.json) | `74af40a86d09126e5e2c3b220102411c44407fd5` | ARMv7, Rovio Fusion, JNI e mixer |
| [RCRDX](../../ports/rcrdx-nextos/README.md) · [SOURCE-MAP](../../ports/rcrdx-nextos/SOURCE-MAP.json) | `7d79b145f9c271ac5bbf959fb254880482ac99a1` | AArch64, callbacks de superfície, `nativeInit` e `SDL_main` |

Comece pelo [main de Action Squad](../../ports/actionsquad-nextos/upstream/src/main.c), [main de KOTOR](../../ports/kotor-nextos/upstream/src/main.c), [main de Angry Birds](../../ports/angrybirds-nextos/upstream/src/main_angrybirds.c) ou [main de RCRDX](../../ports/rcrdx-nextos/upstream/src/main.c). Eles representam fluxos diferentes; não há uma lista universal de entrypoints que sirva aos quatro.

## 3. Distinguir os caminhos de entrada

| Caminho encontrado | Contrato necessário |
| --- | --- |
| Activity Java com métodos JNI | Classes, assinaturas, `JNI_OnLoad`/registro e callbacks realmente chamados |
| NativeActivity | Estrutura e callbacks `ANativeActivity`, lifecycle, janela, input e estado salvo |
| `android_native_app_glue` | Thread do app, comandos, looper, sincronização e entrada `android_main` da build |
| SDL Android | JNI da Activity/áudio/controle, surface/dimensões, thread de `nativeRunMain` e loop SDL |

A documentação NDK descreve `ANativeActivity_onCreate` e o registro de callbacks de Activity. Reconstrua o contrato observado antes de fornecer estruturas ou disparar eventos. [Conceitos NDK](https://developer.android.com/ndk/guides/concepts). Não substitua a sequência nativa por uma chamada direta ao jogo.

JNI exige ainda referências locais/globais, exceções, strings/arrays, assinaturas e associação de threads. `JNIEnv` é específico da thread; compartilhá-lo entre threads quebra esse contrato. [Orientação JNI oficial](https://developer.android.com/ndk/guides/jni-tips). Modele também callbacks tardios, pausa, retomada e encerramento.

## 4. Implementar a ponte Bionic/Linux

Use [shims](SHIMS.md), os contratos V5 e o [exemplo autoral integrado](../../examples/first-port/README.md). O loader Linux e as bibliotecas Android pertencem a ambientes ABI diferentes. Não use `dlopen` glibc indiscriminadamente para carregar um ELF Bionic.

Registre assinatura, tipo, tamanho/alinhamento, ownership, thread, valor de erro e teste de cada fronteira: libc/libm, C++, pthread/TLS, `errno`, assets, JNI/NDK, áudio e EGL/GLES. Em ARMv7, prove as passagens softfp/hard-float, inclusive callbacks. Imports obrigatórios desconhecidos, símbolos TLS ou relocações sem suporte V5 precisam de diagnóstico explícito; não “resolva” todos com endereços de funções vazias.

Construa em projeto próprio com o [SDK público](../../toolchains/sdk/README.md). Confira dependências e arquivos omitidos antes de aproveitar scripts históricos. Uma API disponível somente em V6 é uma incompatibilidade a registrar; não altere componentes V5 nem migre referências automaticamente.

## 5. Verificar imagem, áudio e input

| Fronteira | Prova e contraprova |
| --- | --- |
| EGL/GLES | Owner único de janela/contexto/present, dimensões reais e pixels antes do present; contexto morto ou vídeo preto falham |
| GLES antigo | Identificar GLES1/1.1 e matriz fixa antes de adaptar ao alvo GLES2; Swordigo/FF4 não viram GLES2 por renomear imports |
| Áudio | Formato, canais, fila, callback e consumo para OpenSL, AudioTrack ou mixer nativo; pausa/retomada e escuta |
| Input | Fila/callback que o jogo consome, press/release, eixos, foco e lifecycle; evento SDL isolado não basta |
| Saída e saves | Ordem nativa de pausa, flush/save e encerramento; recarregar progresso sem processo residual |

Use SDL do sistema. Não embuta uma SDL privada para copiar uma solução histórica. Um cursor touch precisa de coordenadas corretas, seta polida e separação entre menus e gameplay; não invente ações globais em botões do jogo.

## 6. Preparar extração e fechar os bytes

Crie a receita [NXExtract](NXEXTRACT.md) para APK/splits/OBB completos do dono. Valide identidade Android e payloads críticos, preserve reempacotamentos compatíveis e execute hooks numa instalação realmente limpa. Não distribua bibliotecas originais ou assets. Inclua `INSTALLATION.md` PT/EN com identidade técnica da cópia de referência, layout e instruções.

Audite todos os ELFs Linux públicos para GLIBC ≤ 2.30; preserve launcher V5, UI NXExtract e NXSplash. Siga [testes e entrega](TESTES-E-ENTREGA.md) para congelar o executável comprovado e vincular a entrega aos mesmos bytes. A coleção não revalidou esses jogos nesta atualização, e cada novo alvo exige sua própria evidência autorizada.

## Missão para a IA

```text
Analise somente o jogo Android fornecido e escolha referências públicas
já selecionadas pelo contrato de bootstrap, ABI, renderer, áudio e input.
Mapeie construtores, JNI/NativeActivity/SDL e threads sem pular etapas.
Implemente o adapter separado com erros reais e testes de ABI/ownership.
Prepare a receita NXExtract completa, persistência e evidência dos bytes.
Preserve V5 e fontes históricas; registre recursos ainda incompatíveis.
```
