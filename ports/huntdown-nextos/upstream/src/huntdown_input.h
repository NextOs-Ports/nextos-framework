#ifndef HUNTDOWN_INPUT_H
#define HUNTDOWN_INPUT_H

#include <stdint.h>

/* SDL controllers are normalized to Xbox layout and exposed through
 * Huntdown's own GamePad methods. Managed menus/gameplay keep their native
 * update order; only the Rewired result boundary is intercepted. */
int hd_input_install(uintptr_t il2cpp_base);
void hd_input_poll(void);
int hd_input_exit_requested(void);
void hd_input_shutdown(void);

#endif
