# SDK público de desenvolvimento

[English](README.en.md)

Este SDK constrói código Linux AArch64/ARMv7 com bibliotecas antigas e executa testes de CPU. A receita usa somente fontes públicas; não depende de nenhuma imagem privada NextOS. Alvo do container: `linux/amd64`. A GPU do aparelho continua fora deste ambiente.

## 1. Construir o ambiente

No Linux, prepare Git, Python 3.11 ou posterior, Docker e acesso ao repositório privado. Clone o histórico completo; `--depth 1` pode omitir o commit fixado. Execute da raiz:

```sh
mkdir -p work
docker build --platform linux/amd64 -t nextos-public-sdk:1 toolchains/sdk
docker image inspect nextos-public-sdk:1 --format '{{.Id}}'
```

O build exige rede para as dependências públicas; as execuções abaixo usam `--network none`. Registre o ID da imagem. Não é necessário publicar a imagem em um registry. A primeira construção compila ferramentas e leva mais tempo; o Docker reutiliza suas camadas depois.

## 2. Saber o que foi fixado

[sources.json](sources.json) e [Dockerfile](Dockerfile) fixam o digest Debian Buster, hashes dos índices arquivados Debian/security, wheel CMake 3.22.6, fonte Git 2.45.2 e Python 3.11.9. O APT continua validando assinaturas e hashes de pacotes. A desativação de validade temporal limita-se ao arquivo histórico, não desativa assinatura.

O Git novo resolve o formato dos objetos exigido pelo helper V5. O Python novo interpreta os literais AST usados na autoridade da receita; Python 3.7 não serve para essa etapa. O Git deste SDK é compilado para operações locais, sem transporte HTTP: faça clones/downloads de fontes pelo host antes de montar o checkout.

As versões de todos os pacotes estão em `/opt/nextos-sdk-packages.tsv` dentro da imagem. Headers/bibliotecas SDL2/EGL/GLES2 AArch64 pertencem ao SDK; não são copiados para um port. O runtime usa as bibliotecas do firmware.

## 3. Compilar e auditar AArch64

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   -e NEXTOS_AARCH64_CC=/usr/bin/aarch64-linux-gnu-gcc   -e NEXTOS_SYSROOT=/ nextos-public-sdk:1 bash -c '
set -eu
cmake -S examples/shims-reference -B work/sdk-arm64   -DCMAKE_TOOLCHAIN_FILE=/src/toolchains/linux-aarch64.cmake
cmake --build work/sdk-arm64 --parallel 2
python3 tools/audit_elf.py work/sdk-arm64/shim-reference-nextos
'
```

Aqui `NEXTOS_SYSROOT=/` descreve a raiz do **container multiarch**, com o compilador cruzado selecionando suas bibliotecas ARM. Não significa usar a raiz do computador ou uma cópia do cartão como SDK. O exemplo testado exigiu GLIBC 2.17.

Para ARMv7, use `NEXTOS_ARMV7_CC=/usr/bin/arm-linux-gnueabihf-gcc`, `toolchains/linux-armv7.cmake`, outro diretório de build e `tools/audit_elf.py --machine ARM`. O exemplo ARMv7 testado exigiu GLIBC 2.4. Isso não prova execução Android ARMv7 nem a ponte softfp.

## 4. Avançar para o primeiro port

Siga o [exemplo integrado](../../examples/first-port/README.md). Ele compila um guest Android freestanding com Clang, sem libc/NDK, e o loader Linux com GCC. Esse perfil pequeno não substitui o NDK para projetos Android que precisam de seus headers, libc ou runtimes C++.

Referências: [Debian arquivado](https://www.debian.org/distrib/archive.en.html), [imagem oficial Debian](https://hub.docker.com/_/debian), [CMake](https://cmake.org/), [fontes Python](https://www.python.org/downloads/release/python-3119/). Consulte [validação e limites](../../publication/ONBOARDING-UPDATE.md).
