# Diagnóstico guiado por falhas reproduzíveis

[English](../en/DIAGNOSTIC-LAB.md)

Execute primeiro os testes do [minijogo autoral](../../examples/first-port/README.md). Cada caso abaixo tem uma falha intencional e uma condição verificável; não exige input comercial.

## Ler o sintoma e escolher a medição

| Caso/log | Resultado esperado | Próxima ação em um port real |
| --- | --- | --- |
| `cpu.log` | Construtor antes de JNI; PASS dos contratos | Preservar sequência e hashes |
| `missing-import.log` | `FAIL resolve`; nenhum construtor | Identificar assinatura e implementar o import; nunca continuar com zero |
| `wrong-package.log` | `NXE3001`; sem marcador | Conferir manifesto e conjunto de splits |
| `wrong-payload.log` | `NXE3001`; payload rejeitado | Comparar build interna/hash antes de offsets |
| `missing-payload.log` | `NXE3001`; seed ausente | Recuperar input completo; não reduzir a receita |
| `wrong-abi.log` | `NXE3001`; biblioteca rejeitada | Conferir ELF, ABI e diretório interno |
| `hook-rollback.log` | `NXE7001`; dados anteriores intactos | Corrigir transformação, preservando o estado aprovado |
| laboratório shader/compute | `TRANSLATION REJECTED`; sem saída | Implementar a operação ou declarar incompatibilidade |

## Distinguir outras fronteiras

Uma versão JNI não admitida exige conferir o contrato do runtime; um JNIEnv compartilhado entre threads está errado mesmo quando o ponteiro não é nulo. TLS/IFUNC/RELR devem aparecer cedo no [inventário](INVENTARIO-ANDROID.md). O core V5 rejeita operações fora de seu contrato; não esconder isso com um resolver permissivo.

No aparelho, áudio sem imagem exige medir o contexto e pixels na fronteira de present. O teste de framebuffer em RAM do laboratório não é essa prova. Texto ausente em Cocos pede verificar bitmap/alpha/stride; transição Unity presa pode ser sincronização, não shader. Consulte os guias por engine e registre uma hipótese por medição.

## Registrar uma correção útil

Anote: comando; arquivo/commit; input técnico; fronteira alcançada; erro; hipótese; mudança mínima; teste que passa; contraprova que deve continuar falhando; resultado físico quando existente. Os logs permanecem privados até revisão. Fonte ou binário novo não herda a aprovação de outro artefato.
