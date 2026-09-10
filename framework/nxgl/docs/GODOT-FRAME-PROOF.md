# Godot frame-proof integration (nxgl 0.3.5; escrito na 0.3.4)

Esta integração é opt-in e source-only. `nxgl_godot_frame_proof.h` é comum a
Godot 3/4, mas cada major mantém seu patch próprio para localizar o contexto,
o present e o resolver reais. Não existe ABI C++ compartilhada entre engines.

A ordem obrigatória é:

1. inicializar o estado com `NXGL_GODOT_FRAME_PROOF_INIT` e chamar
   `nxgl_godot_frame_proof_begin()` no início do construtor, antes de GL poder
   falhar;
2. criar e tornar corrente exatamente um contexto pelo provider declarado;
3. adaptar o resolver desse contexto (por exemplo, `eglGetProcAddress`) e
   chamar `nxgl_godot_frame_proof_context()`;
4. chamar `nxgl_godot_frame_proof_before_swap()` imediatamente antes de cada
   `SDL_GL_SwapWindow` ou `eglSwapBuffers` real e executar o present **somente**
   quando o retorno for 0;
5. chamar `nxgl_godot_frame_proof_stop()` antes de destruir o contexto.

Retorno `NXGL_GODOT_FRAME_PROOF_FATAL` (`-2`) exige que o DisplayServer pare
de apresentar, consuma `nxgl_godot_frame_proof_consume_close()` uma vez e
encerre com `nxgl_godot_frame_proof_exit_status()` (72). Antes de publicar
health deve conferir `nxgl_godot_frame_proof_health_allowed()`. O fatal nunca
volta a 0, mesmo depois do consumo/stop; OK deixa close ausente e status 0.

O wrapper rejeita resolver ausente, dimensão inválida, present anterior ao
contexto, inicialização repetida e uso depois do shutdown. Ele não limpa,
desenha, abre janela nem apresenta quadro. O resolver é indispensável em
providers `RTLD_LOCAL`: sem ele, `dlsym(RTLD_DEFAULT)` pode produzir
`UNMEASURED` mesmo quando o jogo desenhou.

O estado é propriedade da instância do display server e começa sempre com o
inicializador público; memória não inicializada não é uma entrada válida.

Somente `nxport.video_proof=required` descreve esta fronteira. Não declarar
`graphics.evidence_boundary=post-first-present`: esse é outro contrato, com
outro adapter e amostra pós-present.

| Major | Integração de engine |
|---|---|
| Godot 3 | patch próprio encontra o MakeCurrent/present da linha 3.x e chama o wrapper comum |
| Godot 4 | patch próprio encontra o DisplayServer/MakeCurrent/present da linha 4.x e chama o wrapper comum |

Um port fixa a versão exata de Godot e os bytes dos dois adapters nxgl. Nunca
copia um patch de uma major para a outra por semelhança visual.
