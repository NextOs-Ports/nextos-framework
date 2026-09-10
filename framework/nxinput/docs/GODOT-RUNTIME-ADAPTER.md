# Godot runtime adapter (nxinput 0.8.1)

Esta integração é opt-in. A política em C puro é comum a Godot 3/4; o glue
source-only fornecido aqui é deliberadamente identificado como Godot 4 e entra
como fonte no checkout pinado dessa major. Não é uma biblioteca C++
compartilhada: headers internos de Godot não oferecem ABI estável entre majors
nem entre todos os minors.

O port copia `include/nxinput_godot_runtime.h` e os dois arquivos
`engine-glue/nxinput_gptk_godot4_glue.*`, registra descritores semânticos próprios
em `nxinput_gptk_live` e chama os callbacks fornecidos. Antes de ativar um
contexto, valida todos os nomes do `InputMap`; na passagem de nativo para GPTK,
usa o neutral handoff; falha em callback/release marca o lifecycle fatal, bloqueia
health, pede fechamento e define status não zero.

O adapter resolve cena/contexto no começo do frame, antes de consumir o lote SDL.
Em toda troca, ele classifica cada controle fisicamente mantido pela autoridade
do contexto **anterior**: `ACTION`/`null` fica suprimido até release/centro;
`NONE`/`native` continua no caminho nativo até release/centro. Uma segunda troca
antes do neutro preserva a primeira autoridade. Os dois masks são reconstruídos
juntos por `nxinput_godot_handoff_partition`; nunca se zera apenas um deles nem
se alimenta um stick ao novo contexto enquanto qualquer barreira estiver ativa.
O neutro desse handoff é radial em 0,20 exclusivamente para soltar drift; os
valores entregues continuam crus e a deadzone configurável permanece do Godot.

Se LEFT_STICK e RIGHT_STICK apontarem para a mesma ação vetorial, o adapter usa
`nxinput_godot_vector_alias`: guarda cada fonte e entrega o máximo por direção.
Uma fonte neutra nunca solta a outra. Ações de borda compartilhadas entre
botão/stick usam o mesmo `nxinput_godot_action_latch`, portanto somente o último
alias solto produz o release semântico.

Ficam obrigatoriamente no adapter do jogo: descoberta de contexto, nomes de
ações e cenas, co-op/multipad, gatilhos especiais, cursor e qualquer sink direto
da gameplay. `parse_input_event()` é enqueue confirmado, não prova de que um
consumer C#/GDScript reagiu. Essa ligação integra a prova externa congelada do
port.

| Linha | Peça reutilizável | Estado |
|---|---|---|
| Godot 3 | `nxinput_godot_runtime.h` + seam C3 já versionada | core comum; glue GPTK deve ser source-only próprio da major |
| Godot 4 | core comum + `nxinput_gptk_godot4_glue.*` | template opt-in versionado |

Nunca copiar o glue Godot 4 para Godot 3 apenas por semelhança de API. Cada
port fixa a major/minor da engine e compila o adapter correspondente dentro
daquela árvore.
