# Exemplo de shims NextOS

[English](README.en.md)

Este projeto C99 executa testes de contratos pequenos e explícitos. Ele não carrega ELF Android nem implementa um runtime completo. Serve para aprender a organizar um shim antes de integrar código real.

## Arquivos e fluxo

| Arquivo | Função |
| --- | --- |
| [shims.h](shims.h) | Assinaturas, registry tipado e contrato de propriedade |
| [shims.c](shims.c) | Implementações conhecidas; erro para ausência/incompatibilidade |
| [main.c](main.c) | Exercita TLS, assinaturas, buffers e logging |
| [CMakeLists.txt](CMakeLists.txt) | Build C99, pthread e teste host |

`nx_demo_resolve` exige nome e assinatura enumerada. Essa enumeração é fornecida pelo chamador; não lê nem valida o ABI de uma biblioteca Android. O membro correspondente da união deve ser chamado com seu tipo exato.

## Compilar e testar

Partindo da raiz do repositório, com compilador C e CMake instalados:

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Resultado esperado: `explicit-shim-contracts` passa e o programa termina com status zero. Os casos conferem import desconhecido, assinatura divergente, NULL, `errno` independente entre duas threads, capacidade insuficiente, propriedade ausente e tag de logging inválida.

## Contratos e limites

`__errno` encaminha ao TLS do host; não implementa todo o TLS Bionic. O logging usa a assinatura de `__android_log_write`, escreve em stderr e retorna 1 ao entregar ou erro negativo; filtragem Android não está implementada. `nx_demo_property` conhece apenas `demo.name` e nunca deve ser registrado como substituto de `__system_property_get`.

Não acrescente um catch-all que retorne sucesso. Uma extensão precisa declarar semântica, inputs/outputs, ownership, erro e teste que prove o comportamento. Leia [o guia de shims](../../docs/pt-BR/SHIMS.md).

Para ARM, siga [o guia de compilação](../../docs/pt-BR/COMPILAR-ARM.md). O CMake não registra testes no modo cross; build ARM e execução ARM são etapas diferentes. Exemplos completos de loader Android, gráficos/áudio e extração continuam sendo trabalho separado.

Código: GPL-3.0-only. Autoria: **NextOS**.
