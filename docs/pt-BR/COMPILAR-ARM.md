# Compilar para ARM e AArch64

[English](../en/BUILD-ARM.md)

O objetivo é saber qual compilador constrói cada parte, produzir um ELF da ABI correta e interpretar seus requisitos. O [SDK público](../../toolchains/sdk/README.md) agora fornece uma receita testada de build AArch64/ARMv7 e baixa glibc. Comece por ele e pelo [exemplo integrado](../../examples/first-port/README.md). Os caminhos genéricos abaixo servem para quem fornece outro toolchain/sysroot; nenhum build host certifica suporte físico.

## 1. Entender as três compilações

| Produto | Ambiente de build | Onde executa |
| --- | --- | --- |
| Teste C de contratos | Compilador do computador | Host Linux |
| Loader/adapter `<port-id>-nextos` | Cross compiler e sysroot Linux | Linux ARM do aparelho |
| Biblioteca Android autoral | NDK e sysroot Bionic | Android ou loader Android compatível |

Uma biblioteca comercial já vem compilada no APK; criar o port não a recompila. A preferência é `arm64-v8a` quando presente. `armeabi-v7a` exige um processo ARM32 e pode exigir ponte de chamada para o host ARMHF. Nenhuma flag converte um ELF32 em ELF64. A ABI Android ARMv7 usa `softfp` para passagem de ponto flutuante, enquanto a interface Linux ARMHF pode usar registradores VFP. [Documentação de ABIs Android](https://developer.android.com/ndk/guides/abis).

## 2. Confirmar primeiro o exemplo host

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Isso isola erros do exemplo e das ferramentas básicas antes de introduzir cross-compilação. O binário criado aqui tem a arquitetura do computador, mesmo que o diretório se chame `work/host` ou o executável termine em `nextos`.

## 3. Preparar um toolchain Linux consistente

O sysroot deve conter headers, arquivos de início de link, libc e bibliotecas de desenvolvimento da mesma arquitetura e distribuição-alvo. Uma cópia incompleta do cartão do aparelho normalmente não contém esses arquivos de desenvolvimento. Registre origem, versão e digest do SDK/sysroot; não use um nome de imagem privada como instrução de instalação pública.

Os caminhos `/opt/...` abaixo são exemplos a substituir pelos seus caminhos reais. O script não baixa nem instala ferramentas. Para releases públicos, o resultado deve exigir no máximo `GLIBC_2.30`; um GCC recente combinado com bibliotecas recentes não produz compatibilidade antiga só por mudar o texto do manifesto.

## 4. Construir para AArch64

```sh
export NEXTOS_AARCH64_CC=/opt/nextos-toolchain/bin/aarch64-linux-gnu-gcc
export NEXTOS_SYSROOT=/opt/nextos-sysroots/aarch64
test -x "$NEXTOS_AARCH64_CC"
test -d "$NEXTOS_SYSROOT"
"$NEXTOS_AARCH64_CC" --version
"$NEXTOS_AARCH64_CC" -dumpmachine
"$NEXTOS_AARCH64_CC" --sysroot="$NEXTOS_SYSROOT" -print-sysroot
cmake -S examples/shims-reference -B work/aarch64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/linux-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/aarch64 --parallel 2
```

O arquivo [linux-aarch64.cmake](../../toolchains/linux-aarch64.cmake) seleciona Linux/AArch64, o compilador e o sysroot explícitos; busca bibliotecas/headers no alvo e programas de build no host. Esse é o papel das opções de busca de um [toolchain CMake](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html). O exemplo é C; projetos C++ também precisam de compilador C++ e bibliotecas correspondentes, definidos no seu toolchain próprio.

Use um diretório de build novo quando trocar ABI, compilador ou sysroot. O cache CMake pode preservar o compilador anterior. Não aponte para headers ARM com bibliotecas x86 nem copie uma SDL privada para contornar uma falha de detecção.

## 5. Auditar antes de executar

```sh
readelf -h work/aarch64/shim-reference-nextos
readelf -l work/aarch64/shim-reference-nextos
readelf -d work/aarch64/shim-reference-nextos
readelf --version-info work/aarch64/shim-reference-nextos
sha256sum work/aarch64/shim-reference-nextos
```

| Saída | O que conferir |
| --- | --- |
| `-h` | ELF64 e `Machine: AArch64`; o nome do arquivo não prova arquitetura |
| `-l` | Interpretador do Linux disponível no alvo, quando houver `INTERP` |
| `-d` | Todas as dependências `NEEDED` e qualquer RPATH/RUNPATH |
| `--version-info` | Versões exigidas de GLIBC; C++ também exige conferir GLIBCXX/CXXABI |
| SHA-256 | Identidade dos bytes que serão testados |

Audite **todos** os ELFs Linux entregues, inclusive auxiliares e bibliotecas. `GLIBC_2.30` é teto de publicação, não prova suficiente de compatibilidade: arquitetura, kernel, SDL, EGL e áudio continuam relevantes. Não use `ldd` para executar análise de um ELF Android desconhecido; examine primeiro as informações estáticas.

Não rode esse ELF num host x86 como se fosse nativo. Este CMake não registra CTest durante cross-compilação: “nenhum teste encontrado” não significa aprovação ARM. Execução por QEMU, quando preparada explicitamente, deve ser relatada como emulação de CPU. O teste físico de GPU/áudio/input pertence à [trilha de validação](TESTES-E-ENTREGA.md).

## 6. Rota ARMv7 quando necessária

```sh
export NEXTOS_ARMV7_CC=/opt/nextos-toolchain/bin/arm-linux-gnueabihf-gcc
export NEXTOS_SYSROOT=/opt/nextos-sysroots/armhf
test -x "$NEXTOS_ARMV7_CC"
test -d "$NEXTOS_SYSROOT"
"$NEXTOS_ARMV7_CC" -dumpmachine
cmake -S examples/shims-reference -B work/armv7 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/linux-armv7.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/armv7 --parallel 2
readelf -h -A work/armv7/shim-reference-nextos
```

Confira ELF32/ARM e os atributos de chamada. O loader ARMHF e o guest Android ARMv7 podem compartilhar a CPU sem compartilhar o ABI de cada função. Consulte `framework/nxloader/include/nxloader_softfp.h` antes de adaptar callbacks com `float`/`double`; layouts de estruturas e `pthread` também precisam de revisão por ABI.

## 7. Construir uma biblioteca Android autoral

Este pequeno exercício é separado do exemplo Linux e ensina apenas o build NDK. O corpo de `nextos_guest_add` aceita valores cuja soma cabe em `int`. Ele não demonstra JNI, relocações do loader ou execução do guest no handheld.

```sh
mkdir -p work/android-guest
cat > work/android-guest/CMakeLists.txt <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(nextos_android_guest LANGUAGES C)
add_library(nextos_guest SHARED guest.c)
CMAKE
cat > work/android-guest/guest.c <<'C'
int nextos_guest_add(int a, int b) { return a + b; }
C
export NEXTOS_NDK=/opt/android-ndk
test -f "$NEXTOS_NDK/build/cmake/android.toolchain.cmake"
cmake -S work/android-guest -B work/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$NEXTOS_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/android-arm64 --parallel 2
readelf -h -d work/android-arm64/libnextos_guest.so
```

Use uma instalação NDK identificada por versão. O API 21 é usado neste exercício ARM64; o nível real de outro projeto deve seguir suas APIs. Os parâmetros `ANDROID_ABI`, `ANDROID_PLATFORM` e `android.toolchain.cmake` seguem o [guia CMake do NDK](https://developer.android.com/ndk/guides/cmake). Não use esse toolchain para o loader Linux.

## 8. Aplicar ao port real

Leia a receita da referência antes de executar qualquer script. Identifique inputs ausentes, ferramentas externas, flags, bibliotecas e arquivos gerados. Escreva a receita do novo port com pins verificáveis e saída em diretório próprio. Registre comando completo, versão do compilador, sysroot, commit e SHA final. Build bem-sucedido ainda não prova extração, runtime ou instalação.

Para erros `crt1.o`, `cannot find -lc`, “wrong format”, “Exec format error” e GLIBC, consulte [problemas comuns](PROBLEMAS-COMUNS.md). Não remova uma auditoria para esconder a incompatibilidade.
