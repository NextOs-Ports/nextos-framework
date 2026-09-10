# Unity: controles, áudio e lifecycle

[English](../en/INPUT-AND-AUDIO.md)

SDL detecta dispositivos e entrega eventos; a engine precisa consumir esses eventos pelo contrato da build. Uma fila preenchida não prova que o botão chegou à ação. A mesma distinção vale entre PCM produzido e som ouvido.

## 1. Descobrir o consumidor de input

Identifique InControl, Rewired, Input System, métodos IL2CPP próprios ou touch nativo. Registre enum, assinatura, device ID, janela temporal e thread de consumo. Normalize o gamepad pelo firmware/PortMaster antes das ações semânticas do jogo.

| Referência | Contrato que merece leitura |
| --- | --- |
| [Suzy Cube](../cases/suzycube.md) | InControl e eixos já transformados pela SDL |
| [Oceanhorn](../cases/oceanhorn.md) | Rewired por KeyEvent/MotionEvent |
| [Prizefighters 2](../cases/pf2.md) | Input System managed, StateEvent e Mouse contextual |
| [Huntdown](../cases/huntdown.md) | Métodos GamePad específicos por perfil |
| [Party Hard GO](../cases/partyhard.md) | D-pad com uma autoridade, release em contexto/foco |
| [Nameless Cat](../cases/namelesscat.md) | Admissão canônica antes de SDL_Init |

Não copiar keycodes de um jogo como tabela universal. O mesmo A físico não deve produzir simultaneamente confirmar e cancelar. A troca entre menu e gameplay deve soltar estados retidos e preservar a identidade do jogador.

## 2. Preservar cliques curtos

Um DOWN e UP podem ocorrer entre dois frames nativos. Guardar só o estado final perde o clique. Observe origem SDL, entrega Android e consumidor do jogo em janelas completas, incluindo pelo menos o frame posterior ao evento.

Use o [analisador de toque curto](../diagnostico/toque/README.md) para detectar perda, duplicação, ausência de release e cobertura insuficiente em registros explícitos. O analisador não injeta eventos nem converte um log incompleto em prova de falha do jogo.

Implemente a fila/latch conforme a API consumidora, preservando ordem, duração e ownership. Teste clique rápido, segurar, arrastar, soltar, cancelar, perder foco e mudar contexto. Acrescentar DOWN extra para “garantir” o clique pode criar duplicação.

## 3. Cursor contextual

Quando a interface exige touch, use seta clara e movimento contínuo no analógico direito, com deadzone radial, progressão e tempo por frame. R3 clica no contexto de menu; gameplay conserva ações/câmera nativas. Não roubar D-pad ou botão principal para movimentar a seta sem necessidade comprovada.

Transforme coordenadas pelo retângulo de conteúdo real; converta posição e delta Y de forma coerente. Um cursor desenhado no lugar certo pode entregar clique no lugar errado se o sink usa outro espaço.

## 4. Reconstruir o áudio real

Registre backend, objetos/vtables, init/start/pause/stop, callbacks, sample rate, canais, formato e quantidade de frames. FMOD continua sendo o mixer; o adapter transporta PCM por OpenSL, AudioTrack, AAudio ou a interface efetivamente usada.

Não simule sucesso ao abrir uma biblioteca ausente. Não escolha uma taxa “típica” por versão Unity. Diferença de taxa acelera ou desacelera áudio; confundir bytes com frames pode causar underrun, repetição ou avanço excessivo do mixer.

[Sally Face](../cases/sallyface.md) exige a thread FMODAudioDevice.run; [Terraria](../cases/terraria.md) relaciona fmodGetInfo ao DirectByteBuffer; [Merchant of the Skies](../cases/merchantskies.md) demonstra por que falsa presença de AAudio deixa a rota errada ativa.

## 5. Validar lifecycle completo

Teste foco, pause/resume, mudança de cena, reconexão, preferências, save/reload e saída. SELECT+START precisa encerrar pelo fluxo aprovado, com persistência antes de um prazo terminal quando necessário. Não usar `_exit` como atalho para pular save ou inicialização.

Somente logs de fila, chamadas JNI ou amplitude PCM não provam experiência completa. Vincule eventos consumidos, áudio ouvido, imagem e saída ao mesmo executável/perfil de dados. Preserve referências e framework V5 durante o trabalho no novo adapter.
