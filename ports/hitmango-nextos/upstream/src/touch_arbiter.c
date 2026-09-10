#include <string.h>

#include "touch_arbiter.h"

void hgo_touch_arbiter_init(hgo_touch_arbiter_state *state)
{
    memset(state, 0, sizeof *state);
}

enum touch_owner hgo_touch_arbiter_owner(const hgo_touch_arbiter_state *state)
{
    return state->owner;
}

void hgo_touch_arbiter_position(const hgo_touch_arbiter_state *state,
                                float *x, float *y)
{
    if (x)
        *x = state->x;
    if (y)
        *y = state->y;
}

int hgo_touch_arbiter_begin(hgo_touch_arbiter_state *state,
                            enum touch_owner owner, float x, float y)
{
    if (owner == TOUCH_OWNER_NONE || state->owner != TOUCH_OWNER_NONE)
        return 0;
    state->owner = owner;
    state->x = x;
    state->y = y;
    return 1;
}

int hgo_touch_arbiter_move(hgo_touch_arbiter_state *state,
                           enum touch_owner owner, float x, float y)
{
    if (owner == TOUCH_OWNER_NONE || state->owner != owner)
        return 0;
    state->x = x;
    state->y = y;
    return 1;
}

int hgo_touch_arbiter_end(hgo_touch_arbiter_state *state,
                          enum touch_owner owner, float x, float y)
{
    if (owner == TOUCH_OWNER_NONE || state->owner != owner)
        return 0;
    state->x = x;
    state->y = y;
    state->owner = TOUCH_OWNER_NONE;
    return 1;
}

enum touch_owner hgo_touch_arbiter_cancel(hgo_touch_arbiter_state *state)
{
    enum touch_owner owner = state->owner;
    state->owner = TOUCH_OWNER_NONE;
    return owner;
}
