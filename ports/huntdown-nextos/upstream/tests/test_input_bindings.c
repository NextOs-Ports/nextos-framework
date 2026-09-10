#include "huntdown_input_bindings.h"

#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message) {
  if (condition) return;
  fprintf(stderr, "huntdown input contract failed: %s\n", message);
  exit(1);
}

int main(void) {
  unsigned dash = hd_gameplay_binding("B9");
  unsigned fire = hd_gameplay_binding("Fire");

  require((dash & HD_SIGNAL_R2) != 0, "R2 must trigger the live B9 dash slot");
  require((dash & HD_SIGNAL_R3) != 0, "R3 must retain the live B9 dash slot");
  require((fire & HD_SIGNAL_R2) == 0, "R2 must not trigger Fire");
  require((fire & (HD_SIGNAL_X | HD_SIGNAL_RB)) ==
              (HD_SIGNAL_X | HD_SIGNAL_RB),
          "X and RB must retain Fire");
  require((hd_gameplay_binding("Jump") & HD_SIGNAL_A) != 0,
          "A gameplay binding changed");
  require((hd_gameplay_binding("Fire2") & HD_SIGNAL_B) != 0,
          "B gameplay binding changed");

  require(hd_menu_binding(HD_MENU_START) == HD_SIGNAL_A,
          "A must confirm in menus");
  require(hd_menu_binding(HD_MENU_ACTION1) == HD_SIGNAL_B,
          "B must cancel in menus");
  require(hd_menu_binding(HD_MENU_ACTION2) == HD_SIGNAL_X,
          "X auxiliary menu slot changed");
  require(hd_menu_binding(HD_MENU_BACK) == HD_SIGNAL_Y,
          "Y bounty/info menu slot changed");
  require(hd_menu_binding(HD_MENU_ENTER) == HD_SIGNAL_START,
          "Start menu slot changed");
  require(hd_menu_binding(HD_MENU_EXIT) == HD_SIGNAL_BACK,
          "Back menu slot changed");

  puts("huntdown input contract passed: R2=dash fire=X/RB menus=A/B/X/Y");
  return 0;
}
