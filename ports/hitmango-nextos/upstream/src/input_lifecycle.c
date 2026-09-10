#include <string.h>

#include "input_lifecycle.h"

enum {
    HGO_SUSPEND_FOCUS = 1u << 0,
    HGO_SUSPEND_VISIBILITY = 1u << 1,
};

void hgo_input_lifecycle_init(hgo_input_lifecycle_state *state)
{
    memset(state, 0, sizeof *state);
}

void hgo_input_lifecycle_event(hgo_input_lifecycle_state *state,
                               enum hgo_input_window_event event)
{
    switch (event) {
    case HGO_INPUT_FOCUS_LOST:
        state->suspend_reasons |= HGO_SUSPEND_FOCUS;
        break;
    case HGO_INPUT_FOCUS_GAINED:
        state->suspend_reasons &= ~HGO_SUSPEND_FOCUS;
        break;
    case HGO_INPUT_HIDDEN:
        state->suspend_reasons |= HGO_SUSPEND_VISIBILITY;
        break;
    case HGO_INPUT_SHOWN:
        state->suspend_reasons &= ~HGO_SUSPEND_VISIBILITY;
        break;
    }
}

int hgo_input_lifecycle_suspended(const hgo_input_lifecycle_state *state)
{
    return state->suspend_reasons != 0;
}
