# Verificar um registro de toque curto

[English](README.en.md)

Analisa um JSON finito no host. Não injeta input, não lê aparelhos e não aprova um port. Código e fixtures sintéticas são cópias identificadas em [SOURCE-MAP.json](../../SOURCE-MAP.json).

## Exemplo

```sh
python3 portando_unity/diagnostico/toque/verificar_toque_curto.py \
  portando_unity/diagnostico/toque/fixtures/preservado.json
python3 -m unittest discover -s portando_unity/diagnostico/toque -p 'test_*.py'
```

Esperado: `TRACE_CONSISTENT`, status 0, `evidence_kind: synthetic` e `physical_validation: NOT_ESTABLISHED_BY_THIS_TOOL`. As outras fixtures demonstram perda, duplicação e cobertura incompleta.

## Contrato do registro

Use `schema: unity-short-press/1`, `contract: stateful-press`, `run_id`, `action`, `evidence_kind`, `executable_sha256`, `native_render_enter_us`, `coverage` e `events`. Kind pode ser synthetic, transcription ou runtime; observações não sintéticas exigem hash real do executável.

Registre os stages `origin`, `delivery`, `consumer`; edges DOWN/MOVE/UP/CANCEL; `gesture`, `route`, `owner_context` e `t_us` monotônico. Janelas de cobertura precisam de início/fim, contagem de descartes, completude e fronteira observada. O consumidor precisa observar `game_action_state` ou `game_press_release_callback`; uma fila de input não basta.

Siga [preservado.json](fixtures/preservado.json) para o formato. A entrada é limitada a 4 MiB e 20.000 eventos; precisa de ao menos dois instantes crescentes de entrada em nativeRender. Não declare janela completa quando registros foram perdidos.

## Resultados e saída

| Status | Exit | Interpretação |
| --- | --- | --- |
| TRACE_CONSISTENT | 0 | Registro cobre um toque curto coerente; não prova hardware |
| TRACE_DEFECT | 1 | Sequência observada contém perda/duplicação/soltura inválida |
| INVALID_INPUT | 2 | JSON/contrato inválido |
| INCONCLUSIVE | 3 | Cobertura ou ordem causal insuficiente |
| REVIEW_REQUIRED | 3 | Rota/owner mudou e exige análise |
| NO_SHORT_PRESS_OBSERVED | 3 | O registro não exercitou o caso de toque curto |

Um log que termina no UP pode não cobrir o frame consumidor seguinte. Não diagnosticar “release perdido” sem a janela necessária. As mensagens originais de erro são preservadas; os códigos acima têm a mesma interpretação em português e inglês.

Leia [input e áudio](../../pt-BR/INPUT-E-AUDIO.md) para instrumentar a fronteira correta no **novo** adapter.
