# Primeiro port: minijogo autoral NextOS

[English](README.en.md)

Este exercício liga um guest Android AArch64 ao `nxloader` V5 real. O quadrado, alvo e som são gerados por código. Nenhum jogo comercial é necessário. A execução em CPU e o fluxo de extração foram testados; imagem/som/controle físicos ainda não foram validados.

## 1. Preparar e compilar

Construa primeiro o [SDK público](../../toolchains/sdk/README.md). Execute os comandos da raiz da coleção, usando um diretório novo:

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   nextos-public-sdk:1 python3 examples/first-port/build.py   --output work/my-first-port
```

O script materializa os pins da coleção, compila `guest.c` para o alvo Android ARM64 freestanding e liga `adapter.c` ao nxloader estático e SDL2/GLES2 do sistema. Audita GLIBC, produz um input autoral e gera um projeto pelo nxgenerator. Em caso de erro, preserve o log e os artefatos. Não apague evidência para repetir um resultado como novo.

Não é um APK executável em Android: `training-input.apk` é um container de exercício com manifesto, biblioteca autoral e seed. O guest não usa Bionic completo, C++ ou serviços Java. Para projetos Android comuns, use o NDK conforme o [guia ARM](../../docs/pt-BR/COMPILAR-ARM.md).

## 2. Entender o código e a ordem

| Arquivo | Contrato |
| --- | --- |
| [guest.c](guest.c) | Construtor, JNI_OnLoad, create/resume/step/render/audio/pause/save/destroy |
| [demo.h](demo.h) | ABI autoral e somente os slots JNI GetEnv/GetVersion utilizados |
| [adapter.c](adapter.c) | Registry explícito, SDL2/GLES2, gamepad, fila PCM e persistência |
| [prepare_seed.py](prepare_seed.py) | Hook transacional que converte `seed=7` para `7` |
| [build.py](build.py) | Build, identidade dos bytes e manifesto de geração |
| [test_pipeline.py](test_pipeline.py) | Testes positivos e negativos do fluxo real |

Ordem: mapear → relocação → imports → proteção → construtor → JNI_OnLoad → create → resume → frames/áudio/input → pause → save → destroy. Import obrigatório ausente impede o construtor. `GetEnv` só admite a thread proprietária e a versão declarada; outras funções JNI não estão implementadas. Não há JVM.

O registro exporta `__android_log_write` e `__errno`, ambos com implementação explícita. A renderização produz RGBA; o adapter faz upload e apresentação GLES2 e rejeita centro preto antes do present. Esse teste foi escolhido para a imagem não preta deste exercício e não é um detector universal. O áudio é mono PCM S16/48 kHz. Pause/foco e hotplug são tratados pelo adapter, mas dependem de prova física.

## 3. Testar a instalação e execução de CPU

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   nextos-public-sdk:1 python3 examples/first-port/test_pipeline.py   work/my-first-port
```

Resultado esperado: `PASS: 11 integrated checks`. O teste realiza extração limpa, hook, validação de saídas, execução AArch64 por QEMU, rejeição de import ausente antes dos construtores, aceitação de reempacotamento e rejeição de package/payload/ABI errados ou payload ausente. Um hook que falha preserva os dados e marcador anteriores.

Os testes usam explicitamente `--ui none` **somente no laboratório host**. Isso não certifica a UI gráfica nem aprova release. Leia `pipeline-tests/RESULT.json` e os logs individuais. `COMPILED.json` fixa os inputs nativos e hashes para evitar recompilar quando só a preparação documental muda.

## 4. Inspecionar os resultados

```sh
python3 tools/inventory_apk.py work/my-first-port/training-input.apk   --output work/training-inventory.json
```

`generated/` contém o launcher canônico, NXSplash, NXExtract gráfico e membros de runtime fixados pelo gerador. O guest e seed permanecem somente no input autoral. Não altere essa árvore após a geração. `runtime/` é uma instalação de desenvolvimento separada para testes de CPU; recebe INSTALLATION bilíngue com identidade exata. `project/` guarda os manifests completos e a receita.

O adapter de geração permanece marcado como scaffold/não release porque não houve promoção nem teste físico. O [contrato de implementação didática](adapter-contract.json) explica o código existente. O exemplo inicia diretamente no gameplay; a seção `menu` exigida pelo schema é uma declaração reservada, sem teste de menu. O mapping é estático; o exemplo não promete edição GPTK em runtime.

## 5. Passar ao aparelho e ao seu jogo

Em uma sessão com aparelho explicitamente autorizado, valide a instalação gráfica pelo launcher gerado, a NXSplash de cinco segundos, renderer, controle, áudio, save/reload e saída. Não copie a instalação headless como prova de extração gráfica. D-pad/analógico esquerdo/setas movem o quadrado; Start/Back/Escape salva e sai no exercício. Esses bindings não são defaults para outros jogos.

O exemplo ainda não é um ZIP de release e não passou pelos gates de PortMaster/firmware. Para publicar um port, complete contratos, documentação autoral do pacote, framework/runtime exigidos e as provas dos bytes finais conforme [testes e entrega](../../docs/pt-BR/TESTES-E-ENTREGA.md). Preserve UI, pins e fontes congeladas. Adapte seu próprio port em `work/ports/` ou em outro repositório.
