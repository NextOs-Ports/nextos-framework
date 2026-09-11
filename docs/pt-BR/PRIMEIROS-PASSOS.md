# Primeiros passos

[English](../en/GETTING-STARTED.md)

Após o teste C introdutório, siga o [SDK público](../../toolchains/sdk/README.md) e o [primeiro port integrado ARM64](../../examples/first-port/README.md): build, pins, guest Android, NXExtract e testes em QEMU, sem dados comerciais.

Este roteiro começa no clone e termina com um exemplo C executando no seu computador. Depois você terá uma base para pedir à IA um port novo. O catálogo reúne fontes de 44 títulos; cada referência conserva seus próprios requisitos e limites.

## 1. Preparar o computador

Use um ambiente Linux com Git, Python 3.11 ou posterior, CMake 3.20 ou posterior, compilador C99, Make ou Ninja e ferramentas ELF (`readelf`). O projeto C aceita CMake 3.16, mas os comandos deste guia usam recursos da interface de teste disponíveis em versões posteriores. Em Windows, execute os comandos dentro de um ambiente Linux; o uso de GPU do handheld continua sendo um teste separado.

Confira o ambiente antes de instalar dependências do jogo:

```sh
git --version
python3 --version
cmake --version
cc --version
readelf --version
```

## 2. Clonar e conferir a coleção

Enquanto o repositório estiver privado, sua conta precisa ter acesso no GitHub. Use sua autenticação habitual, sem colar tokens em scripts ou documentação.

```sh
git clone https://github.com/NextOs-Ports/nextos-framework.git
cd nextos-framework
git rev-parse HEAD
python3 publication/verify.py
```

Guarde o commit da coleção. O verificador confere os arquivos importados contra seus manifestos e procura formatos/padrões proibidos reconhecidos. Uma falha de hash exige recuperar a fonte correta; não atualize o manifesto apenas para fazer o erro desaparecer.

## 3. Compilar o primeiro exemplo

Todos os comandos abaixo partem da raiz do clone:

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Resultado esperado: o teste `explicit-shim-contracts` passa. O programa demonstra resolução tipada, erro para import desconhecido, `errno` por thread e operações com limites de buffer. [Leia o contrato do exemplo](../../examples/shims-reference/README.md).

Um teste host aprovado confirma somente esse exemplo. Ainda não há carregamento de uma biblioteca Android, imagem no Mali nem jogo executando.

## 4. Separar estudo e desenvolvimento

Leia [AGENTS.md](../../AGENTS.md) e escolha uma referência no [catálogo](../../catalog/README.md). Mantenha a coleção como fonte de consulta e escreva seu port em `work/ports/<port-id>/` ou num repositório separado. `work/` é ignorado pelo Git, mas isso não protege arquivos que você enviar por outros meios: mantenha os dados comerciais fora de commits e uploads.

Antes de programar, defina jogo/build, ABI, engine, sistema/GPU do alvo e o primeiro resultado que deseja comprovar. Dados do dono ficam numa área privada explícita. Nenhum endereço antigo de aparelho autoriza uma conexão nova.

## 5. Escolher a próxima trilha

| Objetivo | Leitura |
| --- | --- |
| Entender os componentes | [Arquitetura](ARQUITETURA.md) |
| Delegar o trabalho de implementação à IA | [Portar com IA](PORTAR-COM-IA.md) |
| Construir um executável Linux ARM | [Compilação ARM](COMPILAR-ARM.md) |
| Implementar imports Android | [Shims](SHIMS.md) |
| Investigar um APK Unity | [Portando Unity](../../portando_unity/README.md) |
| Preparar os dados do dono | [NXExtract e instalação](NXEXTRACT.md) |
| Saber o que pode declarar como aprovado | [Testes e entrega](TESTES-E-ENTREGA.md) |

Consulte [problemas comuns](PROBLEMAS-COMUNS.md) quando o primeiro comando falhar. Informe o comando, o erro e a arquitetura, sem anexar APK ou log privado completo.
