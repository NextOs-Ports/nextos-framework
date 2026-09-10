# Contrato declarativo de ações — `input-contract.json`

Opt-in. Vale para port que tem **bridge de input própria**, isto é, que traduz
controle normalizado em slot interno da engine em vez de usar o `nxinput`
direto. Um port sem o arquivo não é conferido por este contrato.

## Por que existe

O defeito de campo do Huntdown: o controle chegava normalizado como
`TRIGGERRIGHT`, a documentação dizia que aquilo era `Dash`, e a tabela lógica do
adapter mandava para `Fire`. Os três estavam em lugares diferentes e ninguém
comparava um com o outro. A build validada ainda por cima consultava o slot
interno `B9`, não o nome descritivo.

Nada disso é pegável lendo o código sozinho, porque o código está *coerente
consigo mesmo* — o que está errado é a relação entre ele, a documentação e a
intenção.

## Forma

```json
{
  "schema": 1,
  "id": "<port>",
  "adapter": "src/main.c",
  "extraction": {
    "pattern": "BTN\\((?P<control>[A-Z_]+)\\)[^\\n]*?(?P<slot>K_[A-Z0-9_]+)",
    "minimum_pairs": 8
  },
  "groups": {
    "gameplay": {
      "confirm": {"control": "A", "slot": "K_A"},
      "cancel":  {"control": "B", "slot": "K_B"}
    }
  }
}
```

- `control` é o nome **normalizado** do controle. Nunca nome de aparelho, nunca
  marca, nunca rótulo físico impresso na carcaça.
- `slot` é o slot interno que a engine realmente lê — o `B9` da história, não o
  `Dash`.
- `extraction` diz **como ler o adapter**, com dois grupos nomeados. O gate usa
  isso para extrair os pares direto da fonte e comparar com o que o contrato
  declara. É o passo que impede a documentação de derivar do código.
- `groups` separa **menu** de **gameplay** de propósito: um controle pode
  legitimamente significar coisas diferentes nos dois, e provar juntos esconde
  exatamente o tipo de conflito que se quer pegar.

## O que o gate cobra

1. vocabulário fechado de controles normalizados;
2. dentro de um mesmo grupo, um controle não pode servir a duas ações — é o
   caso `R2 = Fire` e `R2 = Dash`;
3. dentro de um mesmo grupo, duas ações não podem apontar para o mesmo slot;
4. todo par declarado tem de existir de fato no adapter, extraído pelo padrão;
5. o padrão precisa render pelo menos `minimum_pairs` pares, para um regex que
   não casa com nada não passar por vazio.

O gate não inventa alias, não escolhe mapeamento por aparelho, não altera o
`nxinput` e não toca em port publicado.

## Complemento runtime 0.5.1

Este contrato declarativo continua descrevendo os sinks reais específicos do
adapter; o framework nunca os deduz. Um port que declara GPTK no modo final
precisa ainda provar a cadeia runtime:

1. `nxinput_gptk_load_at` selecionou owner/default e publicou o SHA-256 exato;
2. parser e allowlist aceitaram todas as ações;
3. o mapping completo SDL2/SDL3/PortMaster foi declarado por controle como
   PRIMARY e qualquer fallback foi suprimido/deduplicado;
4. menu e gameplay foram selecionados por estado real da engine;
5. cada pressão chegou uma vez ao sink declarado (`delivery_count=1`);
6. RIGHT_STICK/R3 controlaram cursor somente no menu e retornaram à câmera/ação
   do jogo em gameplay, sem roubar A ou D-pad;
7. SELECT+START foi observado pelo chord independente do dispatcher do jogo.

Receipt de presença do arquivo não satisfaz esses itens. O receipt final deve
estar ligado ao mesmo ELF, ZIP e generation testados; suporte de aparelho
continua sendo decisão da release, não conteúdo inventado pelo `nxinput`.
