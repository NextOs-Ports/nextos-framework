#ifndef HUNTDOWN_INPUT_BINDINGS_H
#define HUNTDOWN_INPUT_BINDINGS_H

#include <strings.h>

/* Pure logical bindings shared by the runtime and the host-side contract test.
 * A signal is SDL-normalized; no firmware or device name participates here. */
enum hd_input_signal {
  HD_SIGNAL_A       = 1u << 0,
  HD_SIGNAL_B       = 1u << 1,
  HD_SIGNAL_X       = 1u << 2,
  HD_SIGNAL_Y       = 1u << 3,
  HD_SIGNAL_LB      = 1u << 4,
  HD_SIGNAL_RB      = 1u << 5,
  HD_SIGNAL_L2      = 1u << 6,
  HD_SIGNAL_R2      = 1u << 7,
  HD_SIGNAL_L3      = 1u << 8,
  HD_SIGNAL_R3      = 1u << 9,
  HD_SIGNAL_START   = 1u << 10,
  HD_SIGNAL_BACK    = 1u << 11,
  HD_SIGNAL_GUIDE   = 1u << 12
};

static inline unsigned hd_gameplay_binding(const char *name) {
  if (!name) return 0;
  if (!strcasecmp(name, "Start")) return HD_SIGNAL_START;
  if (!strcasecmp(name, "Back")) return HD_SIGNAL_BACK;
  if (!strcasecmp(name, "Jump")) return HD_SIGNAL_A;
  /* The pinned Android build asks for B9, not the descriptive name Dash.
   * Keep the legacy Dash/B route intact and feed R2 into the live B9 slot. */
  if (!strcasecmp(name, "Dash")) return HD_SIGNAL_B;
  if (!strcasecmp(name, "Fire")) return HD_SIGNAL_X | HD_SIGNAL_RB;
  if (!strcasecmp(name, "Fire2"))
    return HD_SIGNAL_B | HD_SIGNAL_LB | HD_SIGNAL_L2;
  if (!strcasecmp(name, "Action")) return HD_SIGNAL_Y;
  if (!strcasecmp(name, "B8")) return HD_SIGNAL_L3;
  if (!strcasecmp(name, "B9")) return HD_SIGNAL_R2 | HD_SIGNAL_R3;
  if (!strcasecmp(name, "B10")) return HD_SIGNAL_GUIDE;
  return 0;
}

enum hd_menu_slot {
  HD_MENU_START,
  HD_MENU_ACTION1,
  HD_MENU_ACTION2,
  HD_MENU_BACK,
  HD_MENU_ENTER,
  HD_MENU_EXIT
};

static inline unsigned hd_menu_binding(enum hd_menu_slot slot) {
  switch (slot) {
    case HD_MENU_START: return HD_SIGNAL_A;
    case HD_MENU_ACTION1: return HD_SIGNAL_B;
    case HD_MENU_ACTION2: return HD_SIGNAL_X;
    case HD_MENU_BACK: return HD_SIGNAL_Y;
    case HD_MENU_ENTER: return HD_SIGNAL_START;
    case HD_MENU_EXIT: return HD_SIGNAL_BACK;
  }
  return 0;
}

#endif
