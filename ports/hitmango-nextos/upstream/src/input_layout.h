#ifndef HGO_INPUT_LAYOUT_H
#define HGO_INPUT_LAYOUT_H

typedef struct {
    int cursor_on_left;
    int cursor_uses_a;
} hgo_input_layout;

#define HGO_INPUT_LAYOUT_V120_INITIALIZER { 1, 1 }

/* Load the public v1.2.0 default while honoring explicit diagnostic opt-outs. */
void hgo_input_layout_from_environment(hgo_input_layout *layout);

int hgo_input_cursor_button_held(const hgo_input_layout *layout,
                                 int native_selection_active,
                                 int a_held, int r3_held);

#endif
