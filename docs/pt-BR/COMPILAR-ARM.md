# Compilar ARM e AArch64

Este primeiro guia permite compilar o exemplo de shims e explica a separação entre Android e Linux. Os ambientes públicos de baixa glibc e as receitas completas de cada port ainda serão consolidados na revisão seguinte.

## 1. Compile e execute o exemplo no computador

Pré-requisitos: Git, CMake, compilador C e ferramentas de build. Num ambiente Linux já preparado:

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

O teste demonstra contratos do exemplo e recusa de símbolos desconhecidos. Ele não carrega um jogo comercial nem prova vídeo no handheld.

## 2. Escolha o alvo Linux

| Input do jogo | Loader Linux | Atenção |
| --- | --- | --- |
| `arm64-v8a` | AArch64 / ELF64 | Rota preferencial |
| `armeabi-v7a` | ARMv7 / ELF32 | Usar apenas quando necessário; chamadas Android softfp podem exigir ponte para Linux ARMHF |

Um compilador Linux cross é diferente do Android NDK. O sysroot Linux precisa conter headers e bibliotecas para o sistema que executará o loader. Para uma entrega pública, auditar todos os ELFs e exigir no máximo GLIBC 2.30. Não escolher automaticamente o sysroot recente da máquina e declarar compatibilidade antiga.

## 3. Compile o exemplo para Linux AArch64

Forneça explicitamente seu compilador cross e sysroot Linux já instalado e verificado:

```sh
export NEXTOS_AARCH64_CC=/caminho/toolchain/bin/aarch64-linux-gnu-gcc
export NEXTOS_SYSROOT=/caminho/sysroot-linux-aarch64
cmake -S examples/shims-reference -B work/aarch64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/linux-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/aarch64 --parallel 2
readelf -h work/aarch64/shim-reference-nextos
readelf -l work/aarch64/shim-reference-nextos
readelf -d work/aarch64/shim-reference-nextos
readelf --version-info work/aarch64/shim-reference-nextos
```

Os dois caminhos são escolhas locais explícitas; não são downloads disponibilizados por este rascunho. Conferir `Machine: AArch64`, interpretador Linux, dependências e versões GLIBC. Não executar o ELF ARM diretamente num host x86. Execute no alvo autorizado ou em emulação claramente identificada, sem tratar QEMU como prova de GPU/áudio/input.

Para ARMv7 use `toolchains/linux-armv7.cmake`, `NEXTOS_ARMV7_CC` e um sysroot ARMHF correspondente, em outro diretório de build. Nenhuma mudança de flags converte uma biblioteca Android 32 bits em 64 bits.

## 4. Quando usar o Android NDK

O NDK serve para construir a biblioteca Android de um exemplo autoral, não o loader Linux. Nesse projeto Android, a configuração usa:

```text
CMAKE_TOOLCHAIN_FILE = <NDK>/build/cmake/android.toolchain.cmake
ANDROID_ABI = arm64-v8a
ANDROID_PLATFORM = android-21 (ou API mínima realmente exigida)
```

O projeto Android de demonstração completo está no backlog desta primeira revisão; não existe uma receita de APK implícita no exemplo C deste diretório. Ver [CMake no NDK](https://developer.android.com/ndk/guides/cmake) e [ABIs Android](https://developer.android.com/ndk/guides/abis).

## 5. Compilar um port de verdade

Peça à IA para ler `SOURCE-MAP.json`, o build upstream e as licenças, identificar os inputs ausentes da seleção e preparar uma receita em um novo repositório. Ela deve materializar os componentes V5 com pins verificáveis, gerar o launcher canônico e preservar o adapter nativo. Algumas referências contêm pins antigos, ambientes locais ou omissões: não execute receitas automaticamente e não declare que todos os 45 builds são autossuficientes.

Um ZIP final precisa de fonte e executável congelados, NXExtract funcional, `INSTALLATION.md`, dados ausentes do pacote e os gates aplicáveis. O rascunho da coleção não produz novos ZIPs dos jogos.
