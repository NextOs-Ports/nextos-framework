/* SPDX-License-Identifier: GPL-3.0-only */
/* Dirige a maquina de toque quadro a quadro e imprime a sequencia integrada,
 * uma linha por quadro. Quem julga e' o .sh: aqui nao ha assercao, so' a
 * gravacao do que a engine receberia. */
#include "nxinput_touch_sequence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *phase_name(nxinput_touch_phase phase) {
  switch (phase) {
    case NXINPUT_TOUCH_DOWN: return "DOWN";
    case NXINPUT_TOUCH_MOVE: return "MOVE";
    case NXINPUT_TOUCH_UP:   return "UP";
    default:                 return "-";
  }
}

/* Cada argumento e' um quadro: "p,x,y" com p=1 pressionado, p=0 solto. */
int main(int argc, char **argv) {
  nxinput_touch_state state;
  float threshold = 0.01f;
  int i;

  if (argc > 1 && strncmp(argv[1], "th=", 3) == 0) {
    threshold = (float)atof(argv[1] + 3);
    argv++; argc--;
  }
  nxinput_touch_state_init(&state, threshold);

  for (i = 1; i < argc; ++i) {
    nxinput_touch_intent intent;
    nxinput_touch_event event;
    int pressed = 0;
    double x = 0.0, y = 0.0;

    if (sscanf(argv[i], "%d,%lf,%lf", &pressed, &x, &y) != 3) {
      fprintf(stderr, "quadro invalido: %s\n", argv[i]);
      return 2;
    }
    intent.pressed = pressed;
    intent.x = (float)x;
    intent.y = (float)y;
    event = nxinput_touch_step(&state, &intent);
    printf("%s %d %d %.3f %.3f t%lu\n", phase_name(event.phase), event.count,
           event.previous_or_peak, (double)event.x, (double)event.y,
           event.down_time);
  }
  return 0;
}
