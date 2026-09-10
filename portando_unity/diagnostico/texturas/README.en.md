# Offline texture planner

[Português](README.md)

Generic `portando_unity` tool preserved with its SHA in [SOURCE-MAP.json](../../SOURCE-MAP.json). Reads explicit JSON on the host and writes a report to stdout. It does not open APKs, convert images, connect to devices or measure RSS/FPS.

## Run the synthetic example

```sh
python3 portando_unity/diagnostico/texturas/planejar_etc1.py \
  portando_unity/diagnostico/texturas/exemplo.json
python3 -m unittest discover -s portando_unity/diagnostico/texturas -p 'test_*.py'
```

The first command should exit 0 with `action: PLAN_ONLY_NO_CONVERSION`. The inventory is synthetic; `runtime_validated` stays false and actual memory/FPS remain `NOT_MEASURED`.

## Prepare an inventory

Copy [exemplo.json](exemplo.json) into a work area, retain `schema: unity-texture-plan/1` and use `kind: inventory` for actual observations. Each texture requires `id`, `width`, `height`, `levels`, `source_format`, `role`, `alpha`, `dynamic`; `levels` accepts a valid integer or `full`. IDs are simple aliases, not file paths.

Accepted formats: RGBA8888, RGB888, RGB565, RGBA4444, A8, RGBA16F, ETC1 and ETC1_DUAL. Roles: color, normal, data, depth, render_target, font_sdf, video, unknown. Alpha: opaque, blend, cutout, premultiplied, unknown. Do not use PNG as a resident format.

JSON supports up to 10,000 textures, input up to 8 MiB, dimensions from 1 to 65,536, and rejects duplicate keys/IDs. Meeting those limits does not establish GPU dimension support.

## Interpret results

| Field/code | Meaning |
| --- | --- |
| `potential_saving_bytes` | Mathematical payload difference |
| `CANDIDATE_REQUIRES_SHADER_AND_VISUAL_PROOF` | Candidate still needs shader/upload and visual evidence |
| `NO_PAYLOAD_SAVING` | No calculated saving |
| `REQUIRES_*_ANALYSIS` | Use, alpha, channel, HDR or update needs specific study |
| `KEEP_EXISTING_COMPRESSED_CONTRACT` | Preserve existing compressed representation |

Totals assume every listed image is resident once simultaneously. Calculated RGBA buffering is only a staging scenario, not actual peak memory. Exit 2 means invalid input, not a game error. Original diagnostic messages are in Portuguese; stable codes/fields are explained in both languages.

Read [graphics and ETC1](../../en/GRAPHICS-AND-TEXTURES.md) before running any actual converter. Generic code follows applicable collection terms; no game-data license is granted.
