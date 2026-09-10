#include <stdlib.h>
#include <string.h>

#include "input_layout.h"

static int environment_default_on(const char *name)
{
    const char *value = getenv(name);
    return !value || strcmp(value, "0") != 0;
}

void hgo_input_layout_from_environment(hgo_input_layout *layout)
{
    layout->cursor_on_left = environment_default_on("HGO_SWAP_STICKS");
    layout->cursor_uses_a = environment_default_on("HGO_CLICK_A");
}

int hgo_input_cursor_button_held(const hgo_input_layout *layout,
                                 int native_selection_active,
                                 int a_held, int r3_held)
{
    if (layout->cursor_uses_a)
        return !native_selection_active && a_held;
    return r3_held;
}
