# Planejador offline de texturas

[English](README.en.md)

Ferramenta genérica de `portando_unity`, preservada com SHA em [SOURCE-MAP.json](../../SOURCE-MAP.json). Lê JSON explícito no host e escreve um relatório em stdout. Não abre APK, não converte imagem, não conecta aparelhos e não mede RSS/FPS.

## Executar o exemplo sintético

```sh
python3 portando_unity/diagnostico/texturas/planejar_etc1.py \
  portando_unity/diagnostico/texturas/exemplo.json
python3 -m unittest discover -s portando_unity/diagnostico/texturas -p 'test_*.py'
```

O primeiro comando deve terminar com status 0 e `action: PLAN_ONLY_NO_CONVERSION`. O inventário é sintético; `runtime_validated` permanece falso e memória/FPS reais permanecem `NOT_MEASURED`.

## Preparar um inventário

Copie [exemplo.json](exemplo.json) para uma área de trabalho, mantenha `schema: unity-texture-plan/1` e use `kind: inventory` para observações reais. Cada textura exige `id`, `width`, `height`, `levels`, `source_format`, `role`, `alpha`, `dynamic`; `levels` aceita inteiro válido ou `full`. O ID é um alias simples, não caminho de arquivo.

Formatos aceitos: RGBA8888, RGB888, RGB565, RGBA4444, A8, RGBA16F, ETC1 e ETC1_DUAL. Papéis: color, normal, data, depth, render_target, font_sdf, video, unknown. Alpha: opaque, blend, cutout, premultiplied, unknown. Não usar PNG como formato residente.

O JSON aceita até 10.000 texturas, entrada até 8 MiB, dimensões entre 1 e 65.536 e rejeita chaves/IDs duplicados. Respeitar esses limites não prova que a GPU aceita a dimensão.

## Interpretar o resultado

| Campo/código | Significado |
| --- | --- |
| `potential_saving_bytes` | Diferença matemática de payload |
| `CANDIDATE_REQUIRES_SHADER_AND_VISUAL_PROOF` | Candidato ainda exige shader/upload e prova visual |
| `NO_PAYLOAD_SAVING` | Não há economia calculada |
| `REQUIRES_*_ANALYSIS` | Uso, alpha, canal, HDR ou update exige estudo próprio |
| `KEEP_EXISTING_COMPRESSED_CONTRACT` | Preservar representação comprimida existente |

Os totais assumem todas as imagens listadas residentes uma vez simultaneamente. O buffer RGBA calculado é somente um cenário de staging; não é pico real. Status 2 indica input inválido, não erro do jogo. Mensagens diagnósticas originais estão em português; os códigos/campos são estáveis e explicados nos dois idiomas.

Leia [gráficos e ETC1](../../pt-BR/GRAFICOS-E-TEXTURAS.md) antes de executar qualquer conversor real. Código genérico sob os termos aplicáveis da coleção; nenhuma licença de dados de jogo é concedida.
