# Check a short-press trace

[Português](README.md)

Analyzes finite JSON on the host. It does not inject input, read devices or approve a port. Code and synthetic fixtures are identified copies in [SOURCE-MAP.json](../../SOURCE-MAP.json).

## Example

```sh
python3 portando_unity/diagnostico/toque/verificar_toque_curto.py \
  portando_unity/diagnostico/toque/fixtures/preservado.json
python3 -m unittest discover -s portando_unity/diagnostico/toque -p 'test_*.py'
```

Expected: `TRACE_CONSISTENT`, exit 0, `evidence_kind: synthetic` and `physical_validation: NOT_ESTABLISHED_BY_THIS_TOOL`. Other fixtures demonstrate loss, duplication and incomplete coverage.

## Trace contract

Use `schema: unity-short-press/1`, `contract: stateful-press`, `run_id`, `action`, `evidence_kind`, `executable_sha256`, `native_render_enter_us`, `coverage` and `events`. Kind can be synthetic, transcription or runtime; non-synthetic observations require the actual executable hash.

Record `origin`, `delivery`, `consumer` stages; DOWN/MOVE/UP/CANCEL edges; `gesture`, `route`, `owner_context` and monotonic `t_us`. Coverage windows need start/end, dropped count, completeness and observed boundary. The consumer must observe `game_action_state` or `game_press_release_callback`; an input queue is insufficient.

Follow [preservado.json](fixtures/preservado.json) for the format. Input is limited to 4 MiB and 20,000 events and needs at least two increasing nativeRender entry times. Do not declare complete coverage when records were dropped.

## Results and exit codes

| Status | Exit | Interpretation |
| --- | --- | --- |
| TRACE_CONSISTENT | 0 | Trace covers a coherent short press; does not prove hardware |
| TRACE_DEFECT | 1 | Observed sequence contains loss/duplication/invalid release |
| INVALID_INPUT | 2 | Invalid JSON/contract |
| INCONCLUSIVE | 3 | Insufficient coverage or causal order |
| REVIEW_REQUIRED | 3 | Route/owner changed and needs analysis |
| NO_SHORT_PRESS_OBSERVED | 3 | Trace did not exercise a short press |

A log ending at UP may not cover the following consumer frame. Do not diagnose “lost release” without the required window. Original error messages are preserved; the codes above have the same interpretation in Portuguese and English.

Read [input and audio](../../en/INPUT-AND-AUDIO.md) to instrument the correct boundary in the **new** adapter.
