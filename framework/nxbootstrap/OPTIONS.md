# nxbootstrap declarative options v1

`options` é uma extensão **aditiva** e opt-in do manifesto v2, introduzida no
nxbootstrap 0.6.28. A ausência do campo preserva o launcher anterior **byte a
byte** — e é assim que os ports já publicados continuam válidos.

O campo `language` **não** foi substituído: ele tem forma e semântica próprias
(códigos de idioma, `auto`, `NXPORT_LANGUAGE`) e continua exatamente como
estava. `options` existe para o resto: uma opção que o jogador edita no topo
do launcher e que o adapter lê por variável de ambiente.

## Forma

```json
"options": [
  {
    "id": "shadows",
    "label": "Sombras / Shadows",
    "values": ["auto", "on", "off"],
    "default": "auto",
    "environment": "MEUPORT_SHADOWS"
  }
]
```

- `id`: `^[a-z][a-z0-9-]{0,31}$`, único no manifesto;
- `label`: rótulo humano opcional (padrão: o próprio `id`);
- `values`: 2 a 32 valores, únicos, `^[A-Za-z0-9][A-Za-z0-9._:-]{0,31}$`;
- `default`: obrigatoriamente um dos `values`;
- `environment`: `^[A-Z][A-Z0-9_]{2,63}$`, único, e **não** pode ser um nome
  reservado (`SDL_*`, `LD_*`, `NXPORT_*`, `NXBOOTSTRAP_*`, `GAME_LANGUAGE`,
  `HOME`, `PATH`, ...). No máximo 16 opções por port.

## O que o launcher gera

Um bloco editável por opção, no mesmo formato do idioma:

```sh
# Sombras / Shadows (auto, on, off).
# Edit only this value; adapters opt in through nxport.json.
GAME_OPTION_SHADOWS="auto"
case "$GAME_OPTION_SHADOWS" in
  auto|on|off) ;;
  *) GAME_OPTION_SHADOWS="auto" ;;
esac
MEUPORT_SHADOWS=${MEUPORT_SHADOWS:-$GAME_OPTION_SHADOWS}
case "$MEUPORT_SHADOWS" in
  auto|on|off) ;;
  *) MEUPORT_SHADOWS=$GAME_OPTION_SHADOWS ;;
esac
NXBOOTSTRAP_OPTION_SHADOWS=$MEUPORT_SHADOWS
export MEUPORT_SHADOWS
```

Depois do hook mutável do adapter, cada opção é **reafirmada** a partir de
`NXBOOTSTRAP_OPTION_<ID>`: adapters consomem, nunca redefinem.

Um valor fora da lista — editado à mão no launcher ou herdado do ambiente —
volta ao padrão declarado. Nenhum valor do jogador chega cru ao shell: o
`case` só aceita literais da lista, que a validação já restringiu a caracteres
seguros.

## Fronteiras

- nada aqui escolhe backend, driver, resolução ou aparelho;
- a opção só existe se o manifesto declarar; o launcher nunca inventa opção;
- nxgenerator recusa um launcher que exponha opção não declarada, e o
  nxrelease 0.2.28 exige que o `options` empacotado seja canônico.

Exemplo completo: `examples/nxport-options.example.json`.
Gate: `tests/test_options.py`.

## Estado

O contrato está **fechado no framework**: implementado, documentado, coberto
por gate próprio e provado aditivo. Não há trabalho pendente aqui.

Adoção é decisão de **port novo**, quando houver necessidade concreta. Nenhum
port existente é migrado — e não é dívida: um port sem `options` gera o mesmo
launcher de antes, byte a byte, e continua válido sem tocar em nada.
