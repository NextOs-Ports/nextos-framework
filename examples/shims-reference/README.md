# Shims de referência NextOS

Exemplo C99 pequeno e compilável para a IA aprender a integrar contratos explícitos. **Não é um Android completo nem a base ampla consolidada dos 45 jogos.** As implementações maiores estão nos ports catalogados e no [mapa de shims](../../docs/pt-BR/SHIMS.md).

Implementa uma tabela tipada para `__errno` e um backend de diagnóstico com a assinatura `__android_log_write`, além de uma consulta de propriedade própria do exemplo. O teste verifica assinatura errada, import desconhecido, isolamento de errno entre threads, limite de buffer e erro de logging.

```sh
cmake -S examples/shims-reference -B work/host
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Para cross-compilar, siga o [guia ARM](../../docs/pt-BR/COMPILAR-ARM.md). Não copie callbacks com assinatura errada para o registry do jogo. Não use esta consulta de propriedade como implementação falsa de `__system_property_get`; ela não afirma um Android SDK, dispositivo ou GPU inexistente.

Extensões futuras devem acrescentar contratos reais, fixtures e perfis, sem um catch-all que retorna sucesso. O próximo exemplo Android/NDK e as demonstrações gráficas/áudio/NXExtract completas estão explicitamente no backlog da revisão.

Código do exemplo: GPL-3.0-only. Autoria: **NextOS**.
