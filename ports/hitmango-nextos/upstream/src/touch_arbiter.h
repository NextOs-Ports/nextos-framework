#ifndef HGO_TOUCH_ARBITER_H
#define HGO_TOUCH_ARBITER_H

enum touch_owner {
    TOUCH_OWNER_NONE,
    TOUCH_OWNER_CURSOR,
    TOUCH_OWNER_SWIPE,
    TOUCH_OWNER_SHORTCUT,
};

typedef struct {
    enum touch_owner owner;
    float x;
    float y;
} hgo_touch_arbiter_state;

void hgo_touch_arbiter_init(hgo_touch_arbiter_state *state);
enum touch_owner hgo_touch_arbiter_owner(const hgo_touch_arbiter_state *state);
void hgo_touch_arbiter_position(const hgo_touch_arbiter_state *state,
                                float *x, float *y);
int hgo_touch_arbiter_begin(hgo_touch_arbiter_state *state,
                            enum touch_owner owner, float x, float y);
int hgo_touch_arbiter_move(hgo_touch_arbiter_state *state,
                           enum touch_owner owner, float x, float y);
int hgo_touch_arbiter_end(hgo_touch_arbiter_state *state,
                          enum touch_owner owner, float x, float y);
enum touch_owner hgo_touch_arbiter_cancel(hgo_touch_arbiter_state *state);

#endif
