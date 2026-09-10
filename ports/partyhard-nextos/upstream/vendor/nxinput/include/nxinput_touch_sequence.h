/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_touch_sequence.h -- maquina de estados do toque sintetico de um
 * ponteiro, pura e testavel.
 *
 * Por que existe: um adapter que sintetiza toque para uma engine Android
 * precisa entregar a sequencia INTEGRADA -- DOWN, os MOVE do meio, e o UP --
 * com o mesmo identificador de gesto do inicio ao fim. Errar essa sequencia
 * nao aparece num clique: o Brotato 1.0.4 preservava DOWN e UP, removia o
 * MOVE e prendia a subida a coordenada inicial. Clique simples ate' parecia
 * funcionar em alguns firmwares, mas segurar e arrastar nunca poderia rolar
 * uma lista.
 *
 * E o teste que existia exercitava so' o helper de coordenadas -- por isso
 * CODIFICOU a regressao em vez de detecta-la. Testar a camada errada e' pior
 * do que nao testar: da' a sensacao de cobertura.
 *
 * O que esta maquina garante, e o que o gate cobra dela:
 *
 *   * um unico ponteiro. Nunca sai DOWN seguido de DOWN sem UP no meio, que e'
 *     o estado que trava a engine achando que o dedo nunca subiu;
 *   * `down_time` constante durante todo o gesto, e so' mudando no proximo
 *     DOWN. E' por ele que a engine distingue um arrasto de dois toques;
 *   * MOVE emitido quando a coordenada muda acima do limiar, e NAO emitido
 *     quando o tremor fica abaixo -- ruido de analogico nao pode virar
 *     arrasto;
 *   * o UP sai na ultima coordenada conhecida, nunca na inicial;
 *   * a contagem no formato que a MainActivity entrega:
 *     DOWN (1,1), segurando (1,1), UP (0,1), quadro seguinte (0,0).
 *
 * Este cabecalho nao conhece SDL, nao le' ambiente e nao chama nada: recebe a
 * intencao do quadro e devolve o evento daquele quadro. E' de proposito, para
 * o gate poder dirigir um gesto inteiro sem aparelho e sem grafico.
 */
#ifndef NXINPUT_TOUCH_SEQUENCE_H
#define NXINPUT_TOUCH_SEQUENCE_H

#include <stddef.h>

#define NXINPUT_TOUCH_SEQUENCE_CONTRACT 1

typedef enum nxinput_touch_phase {
  NXINPUT_TOUCH_NONE = 0,
  NXINPUT_TOUCH_DOWN = 1,
  NXINPUT_TOUCH_MOVE = 2,
  NXINPUT_TOUCH_UP = 3
} nxinput_touch_phase;

typedef struct nxinput_touch_intent {
  int pressed;   /* o dedo (real ou sintetico) esta' encostado neste quadro */
  float x;
  float y;
} nxinput_touch_intent;

typedef struct nxinput_touch_event {
  nxinput_touch_phase phase;
  int count;              /* o `current` que a MainActivity recebe */
  int previous_or_peak;   /* o segundo argumento dela */
  float x;
  float y;
  unsigned long down_time; /* constante dentro de um gesto */
} nxinput_touch_event;

typedef struct nxinput_touch_state {
  int pressed;
  int previous;
  float x;
  float y;
  float move_threshold;    /* abaixo disto e' tremor, nao arrasto */
  unsigned long down_time;
  unsigned long clock;
} nxinput_touch_state;

static void nxinput_touch_state_init(nxinput_touch_state *state,
                                     float move_threshold) {
  if (state == NULL) {
    return;
  }
  state->pressed = 0;
  state->previous = 0;
  state->x = 0.0f;
  state->y = 0.0f;
  state->move_threshold = move_threshold > 0.0f ? move_threshold : 0.0f;
  state->down_time = 0u;
  state->clock = 0u;
}

static float nxinput_touch_abs(float value) {
  return value < 0.0f ? -value : value;
}

/* Um quadro. Devolve o evento a entregar, ou phase NONE quando nao ha nada a
 * dizer -- e mesmo assim o chamador continua obrigado a bater na engine com
 * (0,0), porque e' esse quadro que limpa os acumuladores dela. */
static nxinput_touch_event nxinput_touch_step(
    nxinput_touch_state *state, const nxinput_touch_intent *intent) {
  nxinput_touch_event event;

  event.phase = NXINPUT_TOUCH_NONE;
  event.count = 0;
  event.previous_or_peak = 0;
  event.x = state != NULL ? state->x : 0.0f;
  event.y = state != NULL ? state->y : 0.0f;
  event.down_time = state != NULL ? state->down_time : 0u;
  if (state == NULL || intent == NULL) {
    return event;
  }

  state->clock += 1u;

  if (intent->pressed) {
    if (!state->pressed) {
      /* DOWN: comeca um gesto novo e carimba o down_time que vai valer ate' o
       * UP. Dois DOWN sem UP no meio sao impossiveis por construcao. */
      state->pressed = 1;
      state->down_time = state->clock;
      state->x = intent->x;
      state->y = intent->y;
      event.phase = NXINPUT_TOUCH_DOWN;
    } else {
      float dx = nxinput_touch_abs(intent->x - state->x);
      float dy = nxinput_touch_abs(intent->y - state->y);
      if (dx > state->move_threshold || dy > state->move_threshold) {
        state->x = intent->x;
        state->y = intent->y;
        event.phase = NXINPUT_TOUCH_MOVE;
      }
      /* abaixo do limiar o dedo continua onde estava: tremor nao arrasta */
    }
    event.count = 1;
  } else if (state->pressed) {
    /* UP na ULTIMA coordenada conhecida, nunca na inicial: prender a subida no
     * ponto de partida e' o que impede rolar uma lista. */
    state->pressed = 0;
    event.phase = NXINPUT_TOUCH_UP;
    event.count = 0;
  }

  event.previous_or_peak = state->previous > event.count ? state->previous
                                                         : event.count;
  event.x = state->x;
  event.y = state->y;
  event.down_time = state->down_time;
  state->previous = event.count;
  return event;
}

#endif /* NXINPUT_TOUCH_SEQUENCE_H */
