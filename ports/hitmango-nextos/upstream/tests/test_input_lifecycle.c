#include <assert.h>
#include <stdio.h>

#include "input_lifecycle.h"

int main(void)
{
    hgo_input_lifecycle_state state;
    hgo_input_lifecycle_init(&state);
    assert(!hgo_input_lifecycle_suspended(&state));

    /* Visibility-only backends may restore without a focus event. */
    hgo_input_lifecycle_event(&state, HGO_INPUT_HIDDEN);
    assert(hgo_input_lifecycle_suspended(&state));
    hgo_input_lifecycle_event(&state, HGO_INPUT_SHOWN);
    assert(!hgo_input_lifecycle_suspended(&state));

    /* SHOWN must not erase an independent focus-loss boundary. */
    hgo_input_lifecycle_event(&state, HGO_INPUT_FOCUS_LOST);
    hgo_input_lifecycle_event(&state, HGO_INPUT_HIDDEN);
    hgo_input_lifecycle_event(&state, HGO_INPUT_SHOWN);
    assert(hgo_input_lifecycle_suspended(&state));
    hgo_input_lifecycle_event(&state, HGO_INPUT_FOCUS_GAINED);
    assert(!hgo_input_lifecycle_suspended(&state));

    /* Duplicate and out-of-order recovery events stay idempotent. */
    for (int cycle = 0; cycle < 1000; cycle++) {
        hgo_input_lifecycle_event(&state, HGO_INPUT_HIDDEN);
        hgo_input_lifecycle_event(&state, HGO_INPUT_HIDDEN);
        assert(hgo_input_lifecycle_suspended(&state));
        hgo_input_lifecycle_event(&state, HGO_INPUT_SHOWN);
        hgo_input_lifecycle_event(&state, HGO_INPUT_SHOWN);
        assert(!hgo_input_lifecycle_suspended(&state));
    }

    puts("input lifecycle test: OK");
    return 0;
}
