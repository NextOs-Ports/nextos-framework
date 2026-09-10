# Diagnóstico Unity por fronteira

[English](../en/DIAGNOSTICS.md)

Registre **sintoma → medição → causa → reparo → contraprova → limite**. Um arquivo chamado “fix” não autoriza ativar o comportamento em todos os jogos.

| Sintoma | Medição que distingue causas | Referência desta seleção |
| --- | --- | --- |
| Zero chamadas GL | Bootstrap, nativeRender e resolução EGL | Terraria |
| Preto após poucos frames | Present, timestamps e seleção Swappy | Huntdown, perfil 2022 |
| RGB correto, scanout preto | Alpha final e compositor; estado antes/depois | Horizon Chase, Hitman GO |
| Sprite com retângulo colorido | Swizzle, alpha e premultiplicação | Freedom Planet 2 |
| Texto em blocos | Canal R8/SDF e formato físico | Merchant of the Skies |
| Chuva magenta, resto correto | Programas de cena além dos globais | Sally Face |
| Cenário/personagem cortados | Atlas completo e retângulos Sprite | Sally Face |
| Engine viva, fase não inicia | Exceção JNI/managed e dados completos | Bomb Chicken |
| Contexto vivo, imagem perdida após trocar SDL | Provider real e bytes da biblioteca | Nameless Cat |
| Direção presa | KeyEvent/HAT duplicados, foco e release | Party Hard GO |
| Clique deslocado | Espaço do Mouse managed e delta Y | Prizefighters 2 |
| Som acelerado | fmodGetInfo, taxa SDL e bytes por frame | Terraria |
| Mixer não começa | Thread FMODAudioDevice e biblioteca realmente disponível | Sally Face, Merchant of the Skies |

As fichas e fontes estão no [índice](../README.md). Este quadro aponta hipóteses sustentadas pelos casos; não prova a mesma causa no novo jogo.

## Uma execução de diagnóstico

Defina o resultado a comparar, o executável e SHA, input, cena, configurações e janela de observação. Acrescente somente a instrumentação necessária; registre quem observa cada fronteira. Não chamar uma captura DRM preta de prova da imagem do jogo nem tratar um PID como contador de frames.

Quando possível, compare pixels antes do present e a tela efetiva. Preserve probes fora do comportamento padrão da release. Termine uma instância antes de abrir outra e use somente o aparelho autorizado na tarefa.

## Ferramentas incluídas

O [planejador de texturas](../diagnostico/texturas/README.md) calcula payload potencial a partir de inventário; não converte assets ou mede FPS. O [verificador de toque](../diagnostico/toque/README.md) analisa sequências explícitas; não injeta eventos nem mede o consumidor sozinho.

Foram incluídos somente ferramentas genéricas e exemplos sintéticos. Galerias, coletores ligados a sessões, relatórios e fontes de jogos fora da seleção não foram transportados.

## Como encerrar uma hipótese

Se a medição contradiz a hipótese, retire o experimento do novo adapter e registre o resultado. Se confirma, implemente o reparo mínimo e a contraprova, depois teste a cena anterior e a próxima transição. Não apagar saves, trocar dados ou reconstruir o binário aprovado para obter artificialmente um resultado verde.

Um achado parcial permanece parcial. Quando a fonte histórica não está vinculada ao artefato aprovado, marque essa lacuna; esta edição não fabrica uma nova certificação física.
