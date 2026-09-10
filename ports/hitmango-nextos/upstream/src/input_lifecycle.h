#ifndef HGO_INPUT_LIFECYCLE_H
#define HGO_INPUT_LIFECYCLE_H

enum hgo_input_window_event {
    HGO_INPUT_FOCUS_LOST,
    HGO_INPUT_FOCUS_GAINED,
    HGO_INPUT_HIDDEN,
    HGO_INPUT_SHOWN,
};

typedef struct {
    unsigned suspend_reasons;
} hgo_input_lifecycle_state;

void hgo_input_lifecycle_init(hgo_input_lifecycle_state *state);
void hgo_input_lifecycle_event(hgo_input_lifecycle_state *state,
                               enum hgo_input_window_event event);
int hgo_input_lifecycle_suspended(const hgo_input_lifecycle_state *state);

#endif
