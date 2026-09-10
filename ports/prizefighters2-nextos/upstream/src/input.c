/*
 * input.c -- native NextOS pads presented to Unity as one Android Gamepad.
 *
 * SDL's GameController layer normalises Xbox, PlayStation, Nintendo, 8BitDo
 * and the usual USB/Bluetooth pads.  Devices missing from SDL's database get
 * a conservative Linux mapping; if SDL still cannot open one, /dev/input/js*
 * remains as a last-resort path.  The resulting buttons and axes are injected
 * through UnityPlayer.nativeInjectEvent as real Android KeyEvent/MotionEvent
 * objects, preserving Unity's normal Android input flow.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "nx_elf.h"
#include "pad_ordinal_fix.h"
#include "pf2.h"

#define MAX_CONTROLLERS 8
#define MAX_RAW_PADS 8
#define RAW_BUTTONS 32
#define RAW_AXES 8

enum logical_button {
    PAD_A,
    PAD_B,
    PAD_X,
    PAD_Y,
    PAD_L1,
    PAD_R1,
    PAD_L2,
    PAD_R2,
    PAD_BACK,
    PAD_START,
    PAD_L3,
    PAD_R3,
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_BUTTON_COUNT,
};

enum logical_axis {
    PAD_LX,
    PAD_LY,
    PAD_RX,
    PAD_RY,
    PAD_LT,
    PAD_RT,
    PAD_HAT_X,
    PAD_HAT_Y,
    PAD_AXIS_COUNT,
};

typedef struct {
    SDL_GameController *handle;
    SDL_JoystickID instance;
} controller_slot;

typedef struct {
    int fd;
    char path[32];
    unsigned char buttons[RAW_BUTTONS];
    int16_t axes[RAW_AXES];
} raw_pad;

static controller_slot controllers[MAX_CONTROLLERS];
static raw_pad raw_pads[MAX_RAW_PADS];
static unsigned char previous_buttons[PAD_BUTTON_COUNT];
static float previous_axes[PAD_AXIS_COUNT];
static void *inject_event;
static int sdl_ready;
static int force_raw;
static int auto_key;
static int auto_start;
static unsigned long auto_cursor_prime_frame;
static unsigned long auto_cursor_click_frame;
static unsigned long auto_cursor_release_frame;
static int boot_touch_pending;
static int select_gamepad_pending;
static uint64_t select_gamepad_after_ms;
static unsigned char swallowed_boot_buttons[PAD_BUTTON_COUNT];
static unsigned long touch_release_frame;
static unsigned long touch_next_move_frame;
static float boot_touch_x = 640.0f;
static float boot_touch_y = 585.0f;
static void *managed_gamepad;
static void *managed_mouse;
static void *queue_event_method;
static void *(*managed_runtime_invoke)(const void *, void *, void **, void **);
static int managed_gamepad_id;
static int managed_mouse_id;
static int managed_registration_allowed;
static float cursor_x = 640.0f;
static float cursor_y = 360.0f;
static float cursor_speed = 1400.0f;
static float cursor_velocity_x;
static float cursor_velocity_y;
static uint64_t cursor_update_ms;
/* The pointer is kept in the port's 1280x720 design space so its speed and
 * keyboard layout stay identical on every handheld.  There are two output
 * spaces: the physical GL viewport used by InputSystem/our overlay, and
 * Unity's logical Screen used by RectTransformUtility.  On a 640x480 R36S
 * those are 640x480 and 1280x720 respectively, so they must never be mixed. */
static float pointer_screen_width = 1280.0f;
static float pointer_screen_height = 720.0f;
static float pointer_unity_width = 1280.0f;
static float pointer_unity_height = 720.0f;
static uint64_t cursor_visible_until_ms;
static int cursor_menu_visible;
static int cursor_r3_previous;
static int cursor_a_previous;
static int cursor_start_previous;
static int cursor_a_captured;
static int cursor_mouse_previous;
static int cursor_locked;
static void *cursor_hover;
static int keyboard_active;
static int keyboard_uppercase;
static int keyboard_selected;
static int keyboard_character_limit = 31;
static int keyboard_latch_buttons;
static int keyboard_notify_visible;
static char keyboard_text[64];
static unsigned char keyboard_previous_buttons[PAD_BUTTON_COUNT];
static void *keyboard_managed_field;
static int keyboard_managed_kind = -1;

/* 1280x720 top-left coordinates.  The interaction model comes from the
 * proven FF4 naming keyboard, while this layout is drawn by the GLES2-safe
 * scissored-clear renderer used by PF2's pointer. */
static const pf2_keyboard_key keyboard_keys[] = {
    { 171, 382, 86, 52, "Q", 'q', 'Q', PF2_KEY_CHARACTER },
    { 265, 382, 86, 52, "W", 'w', 'W', PF2_KEY_CHARACTER },
    { 359, 382, 86, 52, "E", 'e', 'E', PF2_KEY_CHARACTER },
    { 453, 382, 86, 52, "R", 'r', 'R', PF2_KEY_CHARACTER },
    { 547, 382, 86, 52, "T", 't', 'T', PF2_KEY_CHARACTER },
    { 641, 382, 86, 52, "Y", 'y', 'Y', PF2_KEY_CHARACTER },
    { 735, 382, 86, 52, "U", 'u', 'U', PF2_KEY_CHARACTER },
    { 829, 382, 86, 52, "I", 'i', 'I', PF2_KEY_CHARACTER },
    { 923, 382, 86, 52, "O", 'o', 'O', PF2_KEY_CHARACTER },
    {1017, 382, 86, 52, "P", 'p', 'P', PF2_KEY_CHARACTER },

    { 218, 444, 86, 52, "A", 'a', 'A', PF2_KEY_CHARACTER },
    { 312, 444, 86, 52, "S", 's', 'S', PF2_KEY_CHARACTER },
    { 406, 444, 86, 52, "D", 'd', 'D', PF2_KEY_CHARACTER },
    { 500, 444, 86, 52, "F", 'f', 'F', PF2_KEY_CHARACTER },
    { 594, 444, 86, 52, "G", 'g', 'G', PF2_KEY_CHARACTER },
    { 688, 444, 86, 52, "H", 'h', 'H', PF2_KEY_CHARACTER },
    { 782, 444, 86, 52, "J", 'j', 'J', PF2_KEY_CHARACTER },
    { 876, 444, 86, 52, "K", 'k', 'K', PF2_KEY_CHARACTER },
    { 970, 444, 86, 52, "L", 'l', 'L', PF2_KEY_CHARACTER },

    { 265, 506, 86, 52, "Z", 'z', 'Z', PF2_KEY_CHARACTER },
    { 359, 506, 86, 52, "X", 'x', 'X', PF2_KEY_CHARACTER },
    { 453, 506, 86, 52, "C", 'c', 'C', PF2_KEY_CHARACTER },
    { 547, 506, 86, 52, "V", 'v', 'V', PF2_KEY_CHARACTER },
    { 641, 506, 86, 52, "B", 'b', 'B', PF2_KEY_CHARACTER },
    { 735, 506, 86, 52, "N", 'n', 'N', PF2_KEY_CHARACTER },
    { 829, 506, 86, 52, "M", 'm', 'M', PF2_KEY_CHARACTER },
    { 923, 506,187, 52, "DEL", 0, 0, PF2_KEY_BACKSPACE },

    { 171, 568,188, 58, "SHIFT", 0, 0, PF2_KEY_SHIFT },
    { 371, 568,458, 58, "SPACE", 0, 0, PF2_KEY_SPACE },
    { 841, 568,269, 58, "DONE", 0, 0, PF2_KEY_DONE },
};

typedef struct {
    uint32_t buttons;
    float left_x;
    float left_y;
    float right_x;
    float right_y;
    float left_trigger;
    float right_trigger;
} managed_gamepad_state;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t type;
    uint16_t size_in_bytes;
    uint16_t device_id;
    double time;
    int32_t event_id;
    uint32_t state_format;
    managed_gamepad_state state;
} managed_state_event;

/* Unity.InputSystem.LowLevel.MouseState in the package used by PF2.
 * Its explicit managed layout is 30 bytes: three Vector2 controls followed by
 * buttons, displayIndex and clickCount. */
typedef struct __attribute__((packed)) {
    float position_x;
    float position_y;
    float delta_x;
    float delta_y;
    float scroll_x;
    float scroll_y;
    uint16_t buttons;
    uint16_t display_index;
    uint16_t click_count;
} managed_mouse_state;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint16_t size_in_bytes;
    uint16_t device_id;
    double time;
    int32_t event_id;
    uint32_t state_format;
    managed_mouse_state state;
} managed_mouse_event;

/* Android KeyEvent codes, in logical_button order. */
static const int android_keycode[PAD_BUTTON_COUNT] = {
    96, 97, 99, 100,       /* A, B, X, Y */
    102, 103, 104, 105,    /* L1, R1, L2, R2 */
    109, 108, 106, 107,    /* back, start, L3, R3 */
    19, 20, 21, 22,        /* dpad */
};

static float normalise_axis(int16_t value)
{
    return value < 0 ? (float)value / 32768.0f : (float)value / 32767.0f;
}

static void strongest_axis(float *dst, float value)
{
    if (fabsf(value) > fabsf(*dst))
        *dst = value;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

/* Keep the pointer long enough to read and traverse a complete menu.  It is
 * still an idle cursor, so it gets out of the way once gameplay starts. */
#define CURSOR_IDLE_MS 12000u
#define CURSOR_MENU_MS 30000u

static void show_cursor_for(uint64_t duration_ms)
{
    if (cursor_visible_until_ms == UINT64_MAX)
        return;
    uint64_t deadline = monotonic_ms() + duration_ms;
    if (deadline > cursor_visible_until_ms)
        cursor_visible_until_ms = deadline;
}

static void show_menu_cursor(void)
{
    cursor_menu_visible = 1;
    show_cursor_for(CURSOR_MENU_MS);
}

static void hide_menu_cursor(void)
{
    cursor_menu_visible = 0;
    cursor_hover = NULL;
    if (cursor_visible_until_ms != UINT64_MAX)
        cursor_visible_until_ms = 0;
}

int pf2_input_cursor(float *x, float *y)
{
    if (!keyboard_active && !cursor_menu_visible &&
        monotonic_ms() >= cursor_visible_until_ms)
        return 0;
    if (x)
        *x = cursor_x;
    if (y)
        *y = cursor_y;
    return 1;
}

static void keyboard_copy_text(const char *text)
{
    snprintf(keyboard_text, sizeof keyboard_text, "%s", text ? text : "");
    size_t length = strlen(keyboard_text);
    if (length > (size_t)keyboard_character_limit) {
        length = (size_t)keyboard_character_limit;
        keyboard_text[length] = '\0';
    }
    keyboard_uppercase =
        length == 0 || keyboard_text[length - 1] == ' ';
}

void pf2_input_keyboard_open(const char *initial, int character_limit)
{
    keyboard_character_limit =
        character_limit > 0 && character_limit < (int)sizeof keyboard_text
            ? character_limit : (int)sizeof keyboard_text - 1;
    keyboard_copy_text(initial);
    keyboard_selected = 0;
    keyboard_active = 1;
    keyboard_latch_buttons = 1;
    keyboard_notify_visible = 1;
    keyboard_managed_field = NULL;
    keyboard_managed_kind = -1;
    memset(keyboard_previous_buttons, 0, sizeof keyboard_previous_buttons);
    cursor_x = keyboard_keys[0].x + keyboard_keys[0].w * 0.5f;
    cursor_y = keyboard_keys[0].y + keyboard_keys[0].h * 0.5f;
    cursor_velocity_x = 0.0f;
    cursor_velocity_y = 0.0f;
    cursor_update_ms = 0;
    nx_log("input: soft keyboard opened (text=\"%s\" limit=%d)",
           keyboard_text, keyboard_character_limit);
}

void pf2_input_keyboard_set(const char *text)
{
    keyboard_copy_text(text);
}

void pf2_input_keyboard_hide(void)
{
    if (keyboard_active)
        nx_log("input: soft keyboard hidden by Unity");
    keyboard_active = 0;
    keyboard_latch_buttons = 0;
    keyboard_notify_visible = 0;
    keyboard_managed_field = NULL;
    keyboard_managed_kind = -1;
    show_menu_cursor();
}

int pf2_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const pf2_keyboard_key **keys,
                                size_t *key_count)
{
    if (!keyboard_active)
        return 0;
    if (text && text_size)
        snprintf(text, text_size, "%s", keyboard_text);
    if (uppercase)
        *uppercase = keyboard_uppercase;
    if (selected)
        *selected = keyboard_selected;
    if (keys)
        *keys = keyboard_keys;
    if (key_count)
        *key_count = sizeof keyboard_keys / sizeof *keyboard_keys;
    return 1;
}

typedef struct {
    void *klass;
    void *type_object;
    void *is_focused;
    void *set_text;
    void *deactivate;
    const char *name;
} managed_text_kind;

typedef struct {
    int attempted;
    int ready;
    void *(*domain_get)(void);
    const void **(*domain_get_assemblies)(void *, size_t *);
    void *(*assembly_get_image)(const void *);
    const char *(*image_get_name)(const void *);
    void *(*class_from_name)(const void *, const char *, const char *);
    const void *(*class_get_type)(void *);
    void *(*type_get_object)(const void *);
    void *(*class_get_method)(void *, const char *, int);
    void *(*runtime_invoke)(const void *, void *, void **, void **);
    void *(*object_unbox)(void *);
    uintptr_t (*array_length)(void *);
    void *(*string_new)(const char *);
    void *find_objects;
    managed_text_kind kind[2];
} managed_text_api;

static managed_text_api text_input;

static int managed_text_resolve(void)
{
    if (text_input.attempted)
        return text_input.ready;
    text_input.attempted = 1;

    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
#define TEXT_RESOLVE(field, symbol) \
    text_input.field = (void *)nx_lookup_in(il2cpp, symbol)
    TEXT_RESOLVE(domain_get, "il2cpp_domain_get");
    TEXT_RESOLVE(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    TEXT_RESOLVE(assembly_get_image, "il2cpp_assembly_get_image");
    TEXT_RESOLVE(image_get_name, "il2cpp_image_get_name");
    TEXT_RESOLVE(class_from_name, "il2cpp_class_from_name");
    TEXT_RESOLVE(class_get_type, "il2cpp_class_get_type");
    TEXT_RESOLVE(type_get_object, "il2cpp_type_get_object");
    TEXT_RESOLVE(class_get_method, "il2cpp_class_get_method_from_name");
    TEXT_RESOLVE(runtime_invoke, "il2cpp_runtime_invoke");
    TEXT_RESOLVE(object_unbox, "il2cpp_object_unbox");
    TEXT_RESOLVE(array_length, "il2cpp_array_length");
    TEXT_RESOLVE(string_new, "il2cpp_string_new");
#undef TEXT_RESOLVE
    if (!text_input.domain_get || !text_input.domain_get_assemblies ||
        !text_input.assembly_get_image || !text_input.image_get_name ||
        !text_input.class_from_name || !text_input.class_get_type ||
        !text_input.type_get_object || !text_input.class_get_method ||
        !text_input.runtime_invoke || !text_input.object_unbox ||
        !text_input.array_length || !text_input.string_new)
        return 0;

    size_t count = 0;
    const void **assemblies = text_input.domain_get_assemblies(
        text_input.domain_get(), &count);
    void *core = NULL;
    void *tmp = NULL;
    void *unity_ui = NULL;
    for (size_t i = 0; i < count; i++) {
        void *image = text_input.assembly_get_image(assemblies[i]);
        const char *name =
            image ? text_input.image_get_name(image) : NULL;
        if (!name)
            continue;
        if (strcmp(name, "UnityEngine.CoreModule.dll") == 0)
            core = image;
        else if (strcmp(name, "Unity.TextMeshPro.dll") == 0)
            tmp = image;
        else if (strcmp(name, "UnityEngine.UI.dll") == 0)
            unity_ui = image;
    }
    if (!core)
        return 0;

    void *object =
        text_input.class_from_name(core, "UnityEngine", "Object");
    if (!object)
        return 0;
    text_input.find_objects =
        text_input.class_get_method(object, "FindObjectsOfType", 2);
    if (!text_input.find_objects)
        return 0;

    text_input.kind[0].name = "TMPro.TMP_InputField";
    if (tmp)
        text_input.kind[0].klass =
            text_input.class_from_name(tmp, "TMPro", "TMP_InputField");
    text_input.kind[1].name = "UnityEngine.UI.InputField";
    if (unity_ui)
        text_input.kind[1].klass =
            text_input.class_from_name(
                unity_ui, "UnityEngine.UI", "InputField");

    int usable = 0;
    for (size_t i = 0; i < sizeof text_input.kind /
                            sizeof *text_input.kind; i++) {
        managed_text_kind *kind = &text_input.kind[i];
        if (!kind->klass)
            continue;
        kind->type_object =
            text_input.type_get_object(
                text_input.class_get_type(kind->klass));
        kind->is_focused =
            text_input.class_get_method(kind->klass, "get_isFocused", 0);
        kind->set_text =
            text_input.class_get_method(kind->klass, "set_text", 1);
        kind->deactivate =
            text_input.class_get_method(
                kind->klass, "DeactivateInputField", 0);
        if (kind->type_object && kind->is_focused && kind->set_text)
            usable++;
    }
    text_input.ready = usable != 0;
    if (text_input.ready)
        nx_log("input: managed text bridge ready (%d field type%s)",
               usable, usable == 1 ? "" : "s");
    return text_input.ready;
}

static int managed_text_find_focused(void)
{
    if (keyboard_managed_field && keyboard_managed_kind >= 0)
        return 1;
    if (!managed_text_resolve())
        return 0;

    for (size_t k = 0; k < sizeof text_input.kind /
                            sizeof *text_input.kind; k++) {
        managed_text_kind *kind = &text_input.kind[k];
        if (!kind->type_object || !kind->is_focused || !kind->set_text)
            continue;
        unsigned char include_inactive = 0;
        void *find_args[] = { kind->type_object, &include_inactive };
        void *exception = NULL;
        void *array = text_input.runtime_invoke(
            text_input.find_objects, NULL, find_args, &exception);
        if (exception || !array)
            continue;
        uintptr_t count = text_input.array_length(array);
        if (count > 4096)
            continue;
        void **items =
            (void **)((unsigned char *)array + 4 * sizeof(void *));
        for (uintptr_t i = 0; i < count; i++) {
            if (!items[i])
                continue;
            exception = NULL;
            void *boxed = text_input.runtime_invoke(
                kind->is_focused, items[i], NULL, &exception);
            if (exception || !boxed)
                continue;
            void *value = text_input.object_unbox(boxed);
            if (!value || *(const unsigned char *)value == 0)
                continue;
            keyboard_managed_field = items[i];
            keyboard_managed_kind = (int)k;
            nx_log("input: focused text field -> %s (%p)",
                   kind->name, keyboard_managed_field);
            return 1;
        }
    }
    nx_log("input: no focused managed text field found");
    return 0;
}

static int managed_text_publish(const char *text)
{
    if (!managed_text_find_focused())
        return 0;
    managed_text_kind *kind =
        &text_input.kind[keyboard_managed_kind];
    void *string = text_input.string_new(text ? text : "");
    void *args[] = { string };
    void *exception = NULL;
    (void)text_input.runtime_invoke(
        kind->set_text, keyboard_managed_field, args, &exception);
    if (exception) {
        nx_log("input: %s.set_text raised %p",
               kind->name, exception);
        keyboard_managed_field = NULL;
        keyboard_managed_kind = -1;
        return 0;
    }
    return 1;
}

static int managed_text_commit(const char *text)
{
    if (!managed_text_publish(text))
        return 0;
    managed_text_kind *kind =
        &text_input.kind[keyboard_managed_kind];
    if (!kind->deactivate) {
        nx_log("input: %s.DeactivateInputField unavailable",
               kind->name);
        return 1;
    }
    void *exception = NULL;
    (void)text_input.runtime_invoke(
        kind->deactivate, keyboard_managed_field, NULL, &exception);
    if (exception) {
        nx_log("input: %s.DeactivateInputField raised %p",
               kind->name, exception);
        return 0;
    }
    nx_log("input: committed text to %s (text=\"%s\")",
           kind->name, text ? text : "");
    return 1;
}

typedef struct {
    int attempts;
    int ready;
    void *(*domain_get)(void);
    const void **(*domain_get_assemblies)(void *, size_t *);
    void *(*assembly_get_image)(const void *);
    const char *(*image_get_name)(const void *);
    void *(*class_from_name)(const void *, const char *, const char *);
    void *(*class_get_method)(void *, const char *, int);
    void *(*runtime_invoke)(const void *, void *, void **, void **);
    void *(*object_unbox)(void *);
    uintptr_t (*array_length)(void *);
    void *(*object_get_class)(void *);
    int (*class_is_assignable_from)(void *, void *);
    int32_t (*string_length)(void *);
    const uint16_t *(*string_chars)(void *);
    void *button_class;
    void *all_selectables;
    void *is_interactable;
    void *select;
    void *get_transform;
    void *get_game_object;
    void *is_active;
    void *get_name;
    void *get_screen_width;
    void *get_screen_height;
    void *contains_point;
    void *press;
} ui_cursor_api;

static ui_cursor_api ui;
static uint64_t ui_cursor_retry_after_ms;
static uint64_t ui_screen_refresh_after_ms;
static int ui_screen_known;

static int ui_cursor_resolve(void)
{
    if (ui.ready)
        return 1;

    uint64_t now = monotonic_ms();
    if (now < ui_cursor_retry_after_ms)
        return 0;
    ui_cursor_retry_after_ms = now + 500;
    ui.attempts++;

    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
#define UI_RESOLVE(field, symbol) \
    ui.field = (void *)nx_lookup_in(il2cpp, symbol)
    UI_RESOLVE(domain_get, "il2cpp_domain_get");
    UI_RESOLVE(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    UI_RESOLVE(assembly_get_image, "il2cpp_assembly_get_image");
    UI_RESOLVE(image_get_name, "il2cpp_image_get_name");
    UI_RESOLVE(class_from_name, "il2cpp_class_from_name");
    UI_RESOLVE(class_get_method, "il2cpp_class_get_method_from_name");
    UI_RESOLVE(runtime_invoke, "il2cpp_runtime_invoke");
    UI_RESOLVE(object_unbox, "il2cpp_object_unbox");
    UI_RESOLVE(array_length, "il2cpp_array_length");
    UI_RESOLVE(object_get_class, "il2cpp_object_get_class");
    UI_RESOLVE(class_is_assignable_from, "il2cpp_class_is_assignable_from");
    UI_RESOLVE(string_length, "il2cpp_string_length");
    UI_RESOLVE(string_chars, "il2cpp_string_chars");
#undef UI_RESOLVE
    if (!ui.domain_get || !ui.domain_get_assemblies ||
        !ui.assembly_get_image || !ui.image_get_name ||
        !ui.class_from_name || !ui.class_get_method ||
        !ui.runtime_invoke || !ui.object_unbox || !ui.array_length ||
        !ui.object_get_class || !ui.class_is_assignable_from ||
        !ui.string_length || !ui.string_chars)
        return 0;

    size_t count = 0;
    const void **assemblies =
        ui.domain_get_assemblies(ui.domain_get(), &count);
    void *core = NULL;
    void *unity_ui = NULL;
    void *unity_ui_module = NULL;
    for (size_t i = 0; i < count; i++) {
        void *image = ui.assembly_get_image(assemblies[i]);
        const char *name = image ? ui.image_get_name(image) : NULL;
        if (!name)
            continue;
        if (strcmp(name, "UnityEngine.CoreModule.dll") == 0)
            core = image;
        else if (strcmp(name, "UnityEngine.UI.dll") == 0)
            unity_ui = image;
        else if (strcmp(name, "UnityEngine.UIModule.dll") == 0)
            unity_ui_module = image;
    }
    if (!core || !unity_ui || !unity_ui_module)
        return 0;

    void *selectable =
        ui.class_from_name(unity_ui, "UnityEngine.UI", "Selectable");
    ui.button_class =
        ui.class_from_name(unity_ui, "UnityEngine.UI", "Button");
    void *component =
        ui.class_from_name(core, "UnityEngine", "Component");
    void *game_object =
        ui.class_from_name(core, "UnityEngine", "GameObject");
    void *object =
        ui.class_from_name(core, "UnityEngine", "Object");
    void *screen =
        ui.class_from_name(core, "UnityEngine", "Screen");
    void *rect_utility =
        ui.class_from_name(unity_ui_module, "UnityEngine",
                           "RectTransformUtility");
    if (!selectable || !ui.button_class || !component || !game_object ||
        !object || !screen || !rect_utility)
        return 0;

    ui.all_selectables =
        ui.class_get_method(selectable, "get_allSelectablesArray", 0);
    ui.is_interactable =
        ui.class_get_method(selectable, "IsInteractable", 0);
    ui.select = ui.class_get_method(selectable, "Select", 0);
    ui.get_transform =
        ui.class_get_method(component, "get_transform", 0);
    ui.get_game_object =
        ui.class_get_method(component, "get_gameObject", 0);
    ui.is_active =
        ui.class_get_method(game_object, "get_activeInHierarchy", 0);
    ui.get_name = ui.class_get_method(object, "get_name", 0);
    ui.get_screen_width = ui.class_get_method(screen, "get_width", 0);
    ui.get_screen_height = ui.class_get_method(screen, "get_height", 0);
    ui.contains_point =
        ui.class_get_method(rect_utility, "RectangleContainsScreenPoint", 3);
    /* Button.Press() is Unity's own activation path.  It performs the same
     * active/interactable checks as a native pointer click before invoking the
     * serialized onClick event, so no scene flow is bypassed here. */
    ui.press = ui.class_get_method(ui.button_class, "Press", 0);
    ui.ready = ui.all_selectables && ui.is_interactable && ui.select &&
               ui.get_transform && ui.get_game_object && ui.is_active &&
               ui.get_name && ui.get_screen_width && ui.get_screen_height &&
               ui.contains_point && ui.press;
    if (ui.ready)
        nx_log("input: right-stick uGUI cursor ready (attempt %d)",
               ui.attempts);
    return ui.ready;
}

void pf2_input_set_screen_size(int width, int height)
{
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return;
    if (pointer_screen_width != (float)width ||
        pointer_screen_height != (float)height) {
        pointer_screen_width = (float)width;
        pointer_screen_height = (float)height;
        nx_log("input: pointer maps 1280x720 -> %dx%d GL viewport",
               width, height);
    }
}

static void cursor_to_screen(float *x, float *y)
{
    if (x)
        *x = cursor_x * pointer_screen_width / 1280.0f;
    if (y)
        *y = pointer_screen_height -
             cursor_y * pointer_screen_height / 720.0f;
}

static void ui_refresh_logical_screen(void)
{
    uint64_t now = monotonic_ms();
    if (now < ui_screen_refresh_after_ms)
        return;
    ui_screen_refresh_after_ms = now + 1000;

    void *exception = NULL;
    void *boxed_width =
        ui.runtime_invoke(ui.get_screen_width, NULL, NULL, &exception);
    if (exception || !boxed_width)
        return;
    int width = *(const int32_t *)ui.object_unbox(boxed_width);
    exception = NULL;
    void *boxed_height =
        ui.runtime_invoke(ui.get_screen_height, NULL, NULL, &exception);
    if (exception || !boxed_height)
        return;
    int height = *(const int32_t *)ui.object_unbox(boxed_height);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return;
    if (!ui_screen_known || pointer_unity_width != (float)width ||
        pointer_unity_height != (float)height) {
        pointer_unity_width = (float)width;
        pointer_unity_height = (float)height;
        ui_screen_known = 1;
        nx_log("input: Unity logical Screen is %dx%d", width, height);
    }
}

static void cursor_to_unity_screen(float *x, float *y)
{
    if (x)
        *x = cursor_x * pointer_unity_width / 1280.0f;
    if (y)
        *y = pointer_unity_height -
             cursor_y * pointer_unity_height / 720.0f;
}

static int ui_invoke_bool(void *method, void *object, void **args,
                          void **exception)
{
    void *boxed = ui.runtime_invoke(method, object, args, exception);
    return boxed && !*exception &&
           *(const unsigned char *)ui.object_unbox(boxed) != 0;
}

static const char *ui_object_name(void *game_object, char *buffer,
                                  size_t capacity)
{
    if (!game_object || capacity == 0)
        return "";
    void *exception = NULL;
    void *string =
        ui.runtime_invoke(ui.get_name, game_object, NULL, &exception);
    if (exception || !string)
        return "";
    int32_t length = ui.string_length(string);
    const uint16_t *chars = ui.string_chars(string);
    size_t used = 0;
    while (used + 1 < capacity && used < (size_t)length) {
        uint16_t ch = chars[used];
        buffer[used] = ch >= 0x20 && ch < 0x7f ? (char)ch : '?';
        used++;
    }
    buffer[used] = '\0';
    return buffer;
}

/* Hit-test the game's own active uGUI Buttons.  Selection/highlight and click
 * both stay inside Unity: Selectable.Select() then the exact serialized
 * Button.onClick UnityEvent.  This gives touch-first Android menus a native
 * controller pointer without guessing scene entry points. */
static int ui_cursor_update(int click)
{
    if (!ui_cursor_resolve())
        return 0;

    void *exception = NULL;
    void *array =
        ui.runtime_invoke(ui.all_selectables, NULL, NULL, &exception);
    if (exception || !array)
        return 0;
    uintptr_t count = ui.array_length(array);
    if (count > 4096)
        return 0;

    /* Il2CppArray on arm64: Il2CppObject, bounds, max_length, vector[]. */
    void **items = (void **)((unsigned char *)array + 4 * sizeof(void *));
    void *hit = NULL;
    void *hit_game_object = NULL;
    float screen_point[2];
    ui_refresh_logical_screen();
    cursor_to_unity_screen(&screen_point[0], &screen_point[1]);
    for (uintptr_t i = 0; i < count; i++) {
        void *selectable = items[i];
        if (!selectable)
            continue;
        exception = NULL;
        if (!ui_invoke_bool(ui.is_interactable, selectable, NULL, &exception))
            continue;
        exception = NULL;
        void *game_object =
            ui.runtime_invoke(ui.get_game_object, selectable, NULL, &exception);
        if (exception || !game_object)
            continue;
        exception = NULL;
        if (!ui_invoke_bool(ui.is_active, game_object, NULL, &exception))
            continue;
        exception = NULL;
        void *transform =
            ui.runtime_invoke(ui.get_transform, selectable, NULL, &exception);
        if (exception || !transform)
            continue;
        void *args[] = { transform, screen_point, NULL };
        exception = NULL;
        if (!ui_invoke_bool(ui.contains_point, NULL, args, &exception))
            continue;
        if (!ui.class_is_assignable_from(
                ui.button_class, ui.object_get_class(selectable)))
            continue;
        hit = selectable;
        hit_game_object = game_object;
    }

    if (hit != cursor_hover) {
        cursor_hover = hit;
        if (hit) {
            exception = NULL;
            ui.runtime_invoke(ui.select, hit, NULL, &exception);
            char name[96];
            nx_log("input: cursor hover -> %s",
                   ui_object_name(hit_game_object, name, sizeof name));
        }
    }
    if (!click)
        return hit != NULL;
    if (!hit) {
        nx_log("input: cursor click found no uGUI Button at screen "
               "(%.0f,%.0f), selectables=%lu",
               screen_point[0], screen_point[1], (unsigned long)count);
        return 0;
    }

    exception = NULL;
    ui.runtime_invoke(ui.press, hit, NULL, &exception);
    char name[96];
    fprintf(stderr, "[pf2] cursor clicked \"%s\"%s\n",
            ui_object_name(hit_game_object, name, sizeof name),
            exception ? " (Unity exception)" : "");
    return exception == NULL;
}

static int update_virtual_cursor(const float *axes, float *delta_x,
                                 float *delta_y)
{
    const float deadzone = 0.18f;
    const float smoothing_rate = 18.0f;
    float x = axes[PAD_RX];
    float y = axes[PAD_RY];
    float magnitude = sqrtf(x * x + y * y);
    float old_x = cursor_x;
    float old_y = cursor_y;
    uint64_t now = monotonic_ms();
    float dt = 1.0f / 60.0f;
    if (cursor_update_ms) {
        uint64_t elapsed_ms = now >= cursor_update_ms
            ? now - cursor_update_ms : 0;
        dt = (float)elapsed_ms / 1000.0f;
        if (dt > 0.05f)
            dt = 0.05f;
    }
    cursor_update_ms = now;
    if (delta_x)
        *delta_x = 0.0f;
    if (delta_y)
        *delta_y = 0.0f;
    if (cursor_locked) {
        cursor_velocity_x = 0.0f;
        cursor_velocity_y = 0.0f;
        return 0;
    }

    float target_x = 0.0f;
    float target_y = 0.0f;
    if (magnitude > deadzone) {
        float capped = fminf(magnitude, 1.0f);
        float strength = (capped - deadzone) / (1.0f - deadzone);
        /* Smoothstep gives precise slow movement near the radial deadzone and
         * reaches the configured full speed at the rim. */
        strength = strength * strength * (3.0f - 2.0f * strength);
        target_x = x / magnitude * strength * cursor_speed;
        target_y = y / magnitude * strength * cursor_speed;
    }

    float smoothing = 1.0f - expf(-smoothing_rate * dt);
    cursor_velocity_x += (target_x - cursor_velocity_x) * smoothing;
    cursor_velocity_y += (target_y - cursor_velocity_y) * smoothing;
    if (magnitude <= deadzone && fabsf(cursor_velocity_x) < 0.5f)
        cursor_velocity_x = 0.0f;
    if (magnitude <= deadzone && fabsf(cursor_velocity_y) < 0.5f)
        cursor_velocity_y = 0.0f;
    if (dt <= 0.0f ||
        (cursor_velocity_x == 0.0f && cursor_velocity_y == 0.0f))
        return 0;

    cursor_x += cursor_velocity_x * dt;
    cursor_y += cursor_velocity_y * dt;
    if (cursor_x < 0.0f)
        cursor_x = 0.0f;
    if (cursor_x > 1279.0f)
        cursor_x = 1279.0f;
    if (cursor_y < 0.0f)
        cursor_y = 0.0f;
    if (cursor_y > 719.0f)
        cursor_y = 719.0f;
    if ((cursor_x == 0.0f && cursor_velocity_x < 0.0f) ||
        (cursor_x == 1279.0f && cursor_velocity_x > 0.0f))
        cursor_velocity_x = 0.0f;
    if ((cursor_y == 0.0f && cursor_velocity_y < 0.0f) ||
        (cursor_y == 719.0f && cursor_velocity_y > 0.0f))
        cursor_velocity_y = 0.0f;
    if (delta_x)
        *delta_x = cursor_x - old_x;
    if (delta_y)
        *delta_y = cursor_y - old_y;
    show_menu_cursor();
    return 1;
}

static int ensure_managed_gamepad(void)
{
    if (managed_gamepad)
        return 1;

    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
    void *(*domain_get)(void) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get");
    const void **(*domain_get_assemblies)(void *, size_t *) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    void *(*assembly_get_image)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    const char *(*image_get_name)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_image_get_name");
    void *(*class_from_name)(const void *, const char *, const char *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    void *(*class_get_method)(void *, const char *, int) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    void *(*runtime_invoke)(const void *, void *, void **, void **) =
        nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    void *(*string_new)(const char *) =
        nx_lookup_in(il2cpp, "il2cpp_string_new");
    void *(*object_unbox)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_object_unbox");
    if (!domain_get || !domain_get_assemblies || !assembly_get_image ||
        !image_get_name || !class_from_name || !class_get_method ||
        !runtime_invoke || !string_new || !object_unbox)
        return 0;

    size_t count = 0;
    const void **assemblies =
        domain_get_assemblies(domain_get(), &count);
    void *input_image = NULL;
    for (size_t i = 0; i < count; i++) {
        void *image = assembly_get_image(assemblies[i]);
        const char *name = image ? image_get_name(image) : NULL;
        if (name && strcmp(name, "Unity.InputSystem.dll") == 0) {
            input_image = image;
            break;
        }
    }
    if (!input_image)
        return 0;

    void *input_system_class =
        class_from_name(input_image, "UnityEngine.InputSystem", "InputSystem");
    void *input_device_class =
        class_from_name(input_image, "UnityEngine.InputSystem", "InputDevice");
    if (!input_system_class || !input_device_class)
        return 0;
    void *add_device_method =
        class_get_method(input_system_class, "AddDevice", 3);
    void *get_device_id_method =
        class_get_method(input_device_class, "get_deviceId", 0);
    void *queue_method =
        class_get_method(input_system_class, "QueueEvent", 1);
    if (!add_device_method || !get_device_id_method || !queue_method)
        return 0;

    void *layout = string_new("Gamepad");
    void *name = string_new("NextOS Gamepad");
    void *add_args[] = { layout, name, NULL };
    void *exception = NULL;
    void *device =
        runtime_invoke(add_device_method, NULL, add_args, &exception);
    if (exception || !device) {
        nx_log("input: InputSystem.AddDevice(Gamepad) failed (exception=%p)",
               exception);
        return 0;
    }

    exception = NULL;
    void *boxed_id =
        runtime_invoke(get_device_id_method, device, NULL, &exception);
    if (exception || !boxed_id) {
        nx_log("input: managed Gamepad deviceId failed (exception=%p)",
               exception);
        return 0;
    }
    managed_gamepad_id = *(int32_t *)object_unbox(boxed_id);
    if (managed_gamepad_id <= 0 || managed_gamepad_id > UINT16_MAX) {
        nx_log("input: invalid managed Gamepad deviceId=%d",
               managed_gamepad_id);
        return 0;
    }

    managed_runtime_invoke = runtime_invoke;
    queue_event_method = queue_method;
    managed_gamepad = device;
    nx_log("input: Unity InputSystem Gamepad registered (deviceId=%d)",
           managed_gamepad_id);

    /* The touch-first uGUI menus are driven by Point/Click actions, not by
     * Selectable navigation.  Add an ordinary InputSystem Mouse so the
     * right-stick pointer follows Unity's normal UI event path. */
    void *mouse_layout = string_new("Mouse");
    void *mouse_name = string_new("NextOS Pointer");
    void *mouse_args[] = { mouse_layout, mouse_name, NULL };
    exception = NULL;
    void *mouse =
        runtime_invoke(add_device_method, NULL, mouse_args, &exception);
    if (exception || !mouse) {
        nx_log("input: InputSystem.AddDevice(Mouse) failed (exception=%p)",
               exception);
        return 1;
    }

    exception = NULL;
    boxed_id =
        runtime_invoke(get_device_id_method, mouse, NULL, &exception);
    if (exception || !boxed_id) {
        nx_log("input: managed Mouse deviceId failed (exception=%p)",
               exception);
        return 1;
    }
    managed_mouse_id = *(int32_t *)object_unbox(boxed_id);
    if (managed_mouse_id <= 0 || managed_mouse_id > UINT16_MAX) {
        nx_log("input: invalid managed Mouse deviceId=%d", managed_mouse_id);
        managed_mouse_id = 0;
        return 1;
    }
    managed_mouse = mouse;
    nx_log("input: Unity InputSystem Mouse registered (deviceId=%d)",
           managed_mouse_id);
    return 1;
}

static uint32_t managed_button_bits(const unsigned char *buttons)
{
    uint32_t bits = 0;
    bits |= (uint32_t)(buttons[PAD_UP] != 0) << 0;
    bits |= (uint32_t)(buttons[PAD_DOWN] != 0) << 1;
    bits |= (uint32_t)(buttons[PAD_LEFT] != 0) << 2;
    bits |= (uint32_t)(buttons[PAD_RIGHT] != 0) << 3;
    bits |= (uint32_t)(buttons[PAD_Y] != 0) << 4;
    bits |= (uint32_t)(buttons[PAD_B] != 0) << 5;
    bits |= (uint32_t)(buttons[PAD_A] != 0) << 6;
    bits |= (uint32_t)(buttons[PAD_X] != 0) << 7;
    bits |= (uint32_t)(buttons[PAD_L3] != 0) << 8;
    bits |= (uint32_t)(buttons[PAD_R3] != 0) << 9;
    bits |= (uint32_t)(buttons[PAD_L1] != 0) << 10;
    bits |= (uint32_t)(buttons[PAD_R1] != 0) << 11;
    bits |= (uint32_t)(buttons[PAD_START] != 0) << 12;
    bits |= (uint32_t)(buttons[PAD_BACK] != 0) << 13;
    return bits;
}

static void queue_managed_state(const unsigned char *buttons,
                                const float *axes)
{
    if (!managed_registration_allowed)
        return;
    if (!ensure_managed_gamepad())
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    managed_state_event event = {
        /* FourCC("STAT"), sizeof(StateEvent + GamepadState), device, time. */
        .type = 0x53544154u,
        .size_in_bytes = sizeof event,
        .device_id = (uint16_t)managed_gamepad_id,
        .time = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0,
        .event_id = 0,
        .state_format = 0x47504144u, /* FourCC("GPAD") */
        .state = {
            .buttons = managed_button_bits(buttons),
            .left_x = axes[PAD_LX],
            /* SDL follows screen coordinates (down is positive); Unity's
             * GamepadState stick controls use Cartesian coordinates (up is
             * positive). */
            .left_y = -axes[PAD_LY],
            .right_x = axes[PAD_RX],
            .right_y = -axes[PAD_RY],
            .left_trigger = axes[PAD_LT],
            .right_trigger = axes[PAD_RT],
        },
    };
    _Static_assert(sizeof(managed_gamepad_state) == 28,
                   "Unity GamepadState layout");
    _Static_assert(sizeof(managed_state_event) == 52,
                   "Unity StateEvent layout");

    void *event_pointer = &event;
    void *args[] = { &event_pointer };
    void *exception = NULL;
    (void)managed_runtime_invoke(queue_event_method, NULL, args, &exception);
    if (exception)
        nx_log("input: InputSystem.QueueEvent raised %p", exception);
    else
        nx_log("input: managed state buttons=%#x L(%.2f,%.2f) "
               "R(%.2f,%.2f) T(%.2f,%.2f) queued",
               event.state.buttons, event.state.left_x, event.state.left_y,
               event.state.right_x, event.state.right_y,
               event.state.left_trigger, event.state.right_trigger);
}

static void queue_managed_mouse(float delta_x, float delta_y, int button_down)
{
    if (!managed_registration_allowed)
        return;
    if (!ensure_managed_gamepad() || !managed_mouse)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float screen_x, screen_y;
    cursor_to_screen(&screen_x, &screen_y);
    managed_mouse_event event = {
        /* FourCC("STAT"), sizeof(StateEvent + MouseState), device, time. */
        .type = 0x53544154u,
        .size_in_bytes = sizeof event,
        .device_id = (uint16_t)managed_mouse_id,
        .time = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0,
        .event_id = 0,
        .state_format = 0x4d4f5553u, /* FourCC("MOUS") */
        .state = {
            /* The drawn pointer uses a 1280x720 top-left design space.
             * InputSystem uses the actual bottom-left Unity Screen space. */
            .position_x = screen_x,
            .position_y = screen_y,
            .delta_x = delta_x * pointer_screen_width / 1280.0f,
            .delta_y = -delta_y * pointer_screen_height / 720.0f,
            .scroll_x = 0.0f,
            .scroll_y = 0.0f,
            .buttons = button_down ? 1u : 0u,
            .display_index = 0,
            .click_count = button_down ? 1u : 0u,
        },
    };
    _Static_assert(sizeof(managed_mouse_state) == 30,
                   "Unity MouseState layout");
    _Static_assert(sizeof(managed_mouse_event) == 54,
                   "Unity Mouse StateEvent layout");

    void *event_pointer = &event;
    void *args[] = { &event_pointer };
    void *exception = NULL;
    (void)managed_runtime_invoke(queue_event_method, NULL, args, &exception);
    if (exception)
        nx_log("input: InputSystem Mouse QueueEvent raised %p", exception);
}

static int keyboard_key_at(float x, float y)
{
    for (size_t i = 0; i < sizeof keyboard_keys / sizeof *keyboard_keys; i++) {
        const pf2_keyboard_key *key = &keyboard_keys[i];
        if (x >= key->x && x < key->x + key->w &&
            y >= key->y && y < key->y + key->h)
            return (int)i;
    }
    return -1;
}

static void keyboard_point_to_selected(void)
{
    const pf2_keyboard_key *key = &keyboard_keys[keyboard_selected];
    cursor_x = key->x + key->w * 0.5f;
    cursor_y = key->y + key->h * 0.5f;
}

static void keyboard_move_selection(int dx, int dy)
{
    const pf2_keyboard_key *current = &keyboard_keys[keyboard_selected];
    int current_x = current->x + current->w / 2;
    int current_y = current->y + current->h / 2;
    int best = -1;
    int best_score = INT32_MAX;
    for (size_t i = 0; i < sizeof keyboard_keys / sizeof *keyboard_keys; i++) {
        if ((int)i == keyboard_selected)
            continue;
        const pf2_keyboard_key *candidate = &keyboard_keys[i];
        int candidate_x = candidate->x + candidate->w / 2;
        int candidate_y = candidate->y + candidate->h / 2;
        int along = dx ? (candidate_x - current_x) * dx
                       : (candidate_y - current_y) * dy;
        if (along <= 0)
            continue;
        int across = dx ? abs(candidate_y - current_y)
                        : abs(candidate_x - current_x);
        int score = along * 4 + across;
        if (score < best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best >= 0) {
        keyboard_selected = best;
        keyboard_point_to_selected();
    }
}

static void keyboard_publish_text(void)
{
    (void)managed_text_publish(keyboard_text);
    pf2_jni_soft_input_text(keyboard_text);
    pf2_jni_soft_input_selection((int)strlen(keyboard_text), 0);
}

static void keyboard_backspace(void)
{
    size_t length = strlen(keyboard_text);
    if (!length)
        return;
    keyboard_text[length - 1] = '\0';
    keyboard_uppercase =
        length == 1 || keyboard_text[length - 2] == ' ';
    keyboard_publish_text();
}

static void keyboard_done(void)
{
    nx_log("input: soft keyboard done (text=\"%s\")", keyboard_text);
    keyboard_active = 0;
    keyboard_latch_buttons = 0;
    keyboard_notify_visible = 0;
    show_menu_cursor();
    pf2_jni_soft_input_visible(0);
    pf2_jni_soft_input_text(keyboard_text);
    pf2_jni_soft_input_selection((int)strlen(keyboard_text), 0);
    pf2_jni_soft_input_closed(0);
    (void)managed_text_commit(keyboard_text);
    keyboard_managed_field = NULL;
    keyboard_managed_kind = -1;
}

static void keyboard_activate_key(int index)
{
    if (index < 0 ||
        index >= (int)(sizeof keyboard_keys / sizeof *keyboard_keys))
        return;
    const pf2_keyboard_key *key = &keyboard_keys[index];
    size_t length = strlen(keyboard_text);
    switch (key->action) {
    case PF2_KEY_CHARACTER:
        if (length < (size_t)keyboard_character_limit) {
            keyboard_text[length++] =
                keyboard_uppercase ? key->upper : key->lower;
            keyboard_text[length] = '\0';
            keyboard_uppercase = 0;
            keyboard_publish_text();
        }
        break;
    case PF2_KEY_BACKSPACE:
        keyboard_backspace();
        break;
    case PF2_KEY_SHIFT:
        keyboard_uppercase = !keyboard_uppercase;
        break;
    case PF2_KEY_SPACE:
        if (length < (size_t)keyboard_character_limit) {
            keyboard_text[length++] = ' ';
            keyboard_text[length] = '\0';
            keyboard_uppercase = 1;
            keyboard_publish_text();
        }
        break;
    case PF2_KEY_DONE:
        keyboard_done();
        break;
    }
}

static void keyboard_handle(const unsigned char *buttons, int cursor_moved,
                            int cursor_click)
{
    if (keyboard_notify_visible) {
        keyboard_notify_visible = 0;
        pf2_jni_soft_input_visible(1);
    }
    int hit = keyboard_key_at(cursor_x, cursor_y);
    if (cursor_moved && hit >= 0)
        keyboard_selected = hit;

    if (keyboard_latch_buttons) {
        memcpy(keyboard_previous_buttons, buttons,
               sizeof keyboard_previous_buttons);
        keyboard_latch_buttons = 0;
        return;
    }

#define KEY_EDGE(key) \
    (buttons[(key)] && !keyboard_previous_buttons[(key)])
    if (KEY_EDGE(PAD_LEFT))
        keyboard_move_selection(-1, 0);
    else if (KEY_EDGE(PAD_RIGHT))
        keyboard_move_selection(1, 0);
    if (KEY_EDGE(PAD_UP))
        keyboard_move_selection(0, -1);
    else if (KEY_EDGE(PAD_DOWN))
        keyboard_move_selection(0, 1);

    if (KEY_EDGE(PAD_B))
        keyboard_backspace();
    if (KEY_EDGE(PAD_X))
        keyboard_uppercase = !keyboard_uppercase;

    if (cursor_click) {
        hit = keyboard_key_at(cursor_x, cursor_y);
        if (hit >= 0) {
            keyboard_selected = hit;
            keyboard_activate_key(hit);
        }
    } else if (KEY_EDGE(PAD_A)) {
        keyboard_activate_key(keyboard_selected);
    } else if (KEY_EDGE(PAD_START)) {
        keyboard_done();
    }
#undef KEY_EDGE

    memcpy(keyboard_previous_buttons, buttons,
           sizeof keyboard_previous_buttons);
}

static void load_mapping_file(const char *path)
{
    if (!path || !*path || access(path, R_OK) != 0)
        return;
    int count = SDL_GameControllerAddMappingsFromFile(path);
    if (count >= 0)
        nx_log("input: loaded %d controller mappings from %s", count, path);
    else
        nx_log("input: cannot load controller mappings from %s: %s",
               path, SDL_GetError());
}

static void install_known_mappings(void)
{
    static const char known_guid[] =
        "0300605b100800000100000010010000";
    const char *mapping = getenv("PF2_PAD_MAP");
    const int explicit_mapping = mapping && *mapping;
    const char *portmaster_mapping =
        getenv("SDL_GAMECONTROLLERCONFIG");
    if (!explicit_mapping && portmaster_mapping &&
        strstr(portmaster_mapping, known_guid)) {
        nx_log("input: preserving PortMaster mapping for 0810:0001");
        return;
    }
    if (!mapping || !*mapping)
        mapping =
            "0300605b100800000100000010010000,USB Gamepad,platform:Linux,"
            "a:b2,b:b1,x:b3,y:b0,"
            "leftshoulder:b4,rightshoulder:b5,"
            "lefttrigger:b6,righttrigger:b7,"
            "back:b8,start:b9,leftstick:b10,rightstick:b11,"
            "leftx:a0,lefty:a1,rightx:a3,righty:a2,"
            "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,";
    int result = SDL_GameControllerAddMapping(mapping);
    nx_log("input: NextOS 0810:0001 mapping %s%s",
           result >= 0 ? "ready" : SDL_GetError(),
           explicit_mapping ? " (PF2_PAD_MAP)" : "");
}

static void add_generic_mapping(int index)
{
    if (SDL_IsGameController(index))
        return;

    SDL_Joystick *probe = SDL_JoystickOpen(index);
    if (!probe)
        return;
    int buttons = SDL_JoystickNumButtons(probe);
    int axes = SDL_JoystickNumAxes(probe);
    int hats = SDL_JoystickNumHats(probe);
    SDL_JoystickClose(probe);
    if (buttons < 8 || axes < 2)
        return;

    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    char guid_text[64];
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
    const char *name = SDL_JoystickNameForIndex(index);
    char safe_name[96];
    snprintf(safe_name, sizeof safe_name, "%s",
             name && *name ? name : "Linux Gamepad");
    for (char *p = safe_name; *p; p++)
        if (*p == ',' || *p == ':')
            *p = ' ';

    char mapping[768];
    int used = snprintf(mapping, sizeof mapping,
                        "%s,%s,platform:Linux,"
                        "a:b0,b:b1,x:b2,y:b3,"
                        "leftshoulder:b4,rightshoulder:b5,",
                        guid_text, safe_name);
    if (buttons > 6)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "lefttrigger:b6,");
    if (buttons > 7)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "righttrigger:b7,");
    if (buttons > 8)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "back:b8,");
    if (buttons > 9)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "start:b9,");
    if (buttons > 10)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "leftstick:b10,");
    if (buttons > 11)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "rightstick:b11,");
    used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                     "leftx:a0,lefty:a1,");
    if (axes > 3)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "rightx:a2,righty:a3,");
    if (hats > 0)
        snprintf(mapping + used, sizeof mapping - (size_t)used,
                 "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");

    int result = SDL_GameControllerAddMapping(mapping);
    nx_log("input: generic profile for \"%s\" (%d buttons, %d axes, %d hats): %s",
           safe_name, buttons, axes, hats,
           result >= 0 ? "ready" : SDL_GetError());
}

static int controller_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].handle)
            count++;
    return count;
}

static int instance_is_open(SDL_JoystickID instance)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].handle && controllers[i].instance == instance)
            return 1;
    return 0;
}

static void publish_controller_info(SDL_GameController *controller)
{
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    unsigned vendor = (unsigned)guid.data[4] | (unsigned)guid.data[5] << 8;
    unsigned product = (unsigned)guid.data[8] | (unsigned)guid.data[9] << 8;
    char guid_text[64];
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
    const char *name = SDL_GameControllerName(controller);
    pf2_jni_input_device_info(name && *name ? name : "NextOS Gamepad",
                              (int)vendor, (int)product, guid_text);
}

static void open_controller_index(int index)
{
    if (index < 0 || index >= SDL_NumJoysticks())
        return;
    pad_ordinal_fix_apply(index, "PF2");
    add_generic_mapping(index);
    if (!SDL_IsGameController(index))
        return;

    SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
    if (instance_is_open(instance))
        return;

    int slot = -1;
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (!controllers[i].handle) {
            slot = i;
            break;
        }
    if (slot < 0) {
        nx_log("input: controller limit reached (%d)", MAX_CONTROLLERS);
        return;
    }

    SDL_GameController *controller = SDL_GameControllerOpen(index);
    if (!controller) {
        nx_log("input: cannot open controller %d: %s", index, SDL_GetError());
        return;
    }
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    controllers[slot].handle = controller;
    controllers[slot].instance = SDL_JoystickInstanceID(joystick);

    char guid_text[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid_text,
                              sizeof guid_text);
    nx_log("input: controller connected: %s (instance=%d guid=%s)",
           SDL_GameControllerName(controller)
               ? SDL_GameControllerName(controller) : "Gamepad",
           (int)controllers[slot].instance, guid_text);
    char *active_mapping = SDL_GameControllerMapping(controller);
    if (active_mapping) {
        nx_log("input: active mapping: %s", active_mapping);
        SDL_free(active_mapping);
    }
    if (controller_count() == 1)
        publish_controller_info(controller);
}

static void close_controller_slot(int slot)
{
    if (!controllers[slot].handle)
        return;
    nx_log("input: controller disconnected (instance=%d)",
           (int)controllers[slot].instance);
    SDL_GameControllerClose(controllers[slot].handle);
    controllers[slot].handle = NULL;
    controllers[slot].instance = -1;
}

static void rescan_controllers(void)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].handle &&
            !SDL_GameControllerGetAttached(controllers[i].handle))
            close_controller_slot(i);
    for (int i = 0, n = SDL_NumJoysticks(); i < n; i++)
        open_controller_index(i);
}

static void close_raw_pad(int index)
{
    if (raw_pads[index].fd >= 0)
        close(raw_pads[index].fd);
    memset(&raw_pads[index], 0, sizeof raw_pads[index]);
    raw_pads[index].fd = -1;
}

static void close_all_raw_pads(void)
{
    for (int i = 0; i < MAX_RAW_PADS; i++)
        close_raw_pad(i);
}

static void open_raw_path(int slot, const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    raw_pads[slot].fd = fd;
    snprintf(raw_pads[slot].path, sizeof raw_pads[slot].path, "%s", path);

    unsigned char axes = 0, buttons = 0;
    char name[128] = {0};
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    ioctl(fd, JSIOCGNAME(sizeof name), name);
    nx_log("input: raw fallback %s opened (%u axes, %u buttons)",
           name[0] ? name : path, axes, buttons);
    if (slot == 0)
        pf2_jni_input_device_info(name[0] ? name : "Linux Gamepad",
                                  0, 0, path);
}

static void rescan_raw_pads(void)
{
    if (!force_raw && controller_count() > 0) {
        close_all_raw_pads();
        return;
    }

    const char *forced_path = getenv("PF2_GAMEPAD");
    int wanted = forced_path && *forced_path ? 1 : MAX_RAW_PADS;
    for (int i = 0; i < wanted; i++) {
        char path[32];
        snprintf(path, sizeof path, "%s",
                 forced_path && *forced_path ? forced_path : "");
        if (!path[0])
            snprintf(path, sizeof path, "/dev/input/js%d", i);
        if (raw_pads[i].fd < 0 && access(path, R_OK) == 0)
            open_raw_path(i, path);
    }
}

static void pump_raw_events(raw_pad *pad)
{
    if (pad->fd < 0)
        return;
    struct js_event event;
    for (;;) {
        ssize_t got = read(pad->fd, &event, sizeof event);
        if (got == (ssize_t)sizeof event) {
            int type = event.type & ~JS_EVENT_INIT;
            if (type == JS_EVENT_BUTTON && event.number < RAW_BUTTONS)
                pad->buttons[event.number] = event.value != 0;
            else if (type == JS_EVENT_AXIS && event.number < RAW_AXES)
                pad->axes[event.number] = event.value;
            continue;
        }
        if (got == 0 ||
            (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            nx_log("input: raw pad %s disconnected%s%s", pad->path,
                   got < 0 ? ": " : "", got < 0 ? strerror(errno) : "");
            int slot = (int)(pad - raw_pads);
            close_raw_pad(slot);
        }
        break;
    }
}

static void merge_raw_pad(const raw_pad *pad, unsigned char *buttons,
                          float *axes)
{
    if (pad->fd < 0)
        return;

    /* Known-good 0810:0001 ordering used by the approved GTA ports. */
    static const unsigned char raw_to_logical[12] = {
        PAD_Y, PAD_B, PAD_A, PAD_X, PAD_L1, PAD_R1,
        PAD_L2, PAD_R2, PAD_BACK, PAD_START, PAD_L3, PAD_R3,
    };
    for (int i = 0; i < 12; i++)
        buttons[raw_to_logical[i]] |= pad->buttons[i] != 0;

    strongest_axis(&axes[PAD_LX], normalise_axis(pad->axes[0]));
    strongest_axis(&axes[PAD_LY], normalise_axis(pad->axes[1]));
    strongest_axis(&axes[PAD_RX], normalise_axis(pad->axes[3]));
    strongest_axis(&axes[PAD_RY], normalise_axis(pad->axes[2]));

    int hat_x = pad->axes[4] < -16384 ? -1 :
                pad->axes[4] > 16384 ? 1 : 0;
    int hat_y = pad->axes[5] < -16384 ? -1 :
                pad->axes[5] > 16384 ? 1 : 0;
    buttons[PAD_LEFT]  |= hat_x < 0;
    buttons[PAD_RIGHT] |= hat_x > 0;
    buttons[PAD_UP]    |= hat_y < 0;
    buttons[PAD_DOWN]  |= hat_y > 0;
    strongest_axis(&axes[PAD_HAT_X], (float)hat_x);
    strongest_axis(&axes[PAD_HAT_Y], (float)hat_y);
    if (pad->buttons[6])
        axes[PAD_LT] = 1.0f;
    if (pad->buttons[7])
        axes[PAD_RT] = 1.0f;
}

static unsigned char controller_button(SDL_GameController *controller,
                                       SDL_GameControllerButton button)
{
    return SDL_GameControllerGetButton(controller, button) != 0;
}

static int controller_guide_pressed(void)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].handle &&
            controller_button(controllers[i].handle,
                              SDL_CONTROLLER_BUTTON_GUIDE))
            return 1;
    return 0;
}

static void exit_to_frontend(void)
{
    nx_log("input: SELECT+START -> exit");
    fflush(NULL);
    sync();
    /* The approved GTA ports use immediate exit here: unwinding a live Mali
     * fbdev context can deadlock inside the proprietary driver. */
    _exit(0);
}

static void merge_controller(SDL_GameController *controller,
                             unsigned char *buttons, float *axes)
{
    static const SDL_GameControllerButton sdl_buttons[PAD_BUTTON_COUNT] = {
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    for (int i = 0; i < PAD_BUTTON_COUNT; i++)
        if (sdl_buttons[i] != SDL_CONTROLLER_BUTTON_INVALID)
            buttons[i] |= controller_button(controller, sdl_buttons[i]);

    strongest_axis(&axes[PAD_LX], normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTX)));
    strongest_axis(&axes[PAD_LY], normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTY)));
    strongest_axis(&axes[PAD_RX], normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTX)));
    strongest_axis(&axes[PAD_RY], normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTY)));

    float lt = normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    float rt = normalise_axis(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    if (lt < 0.0f)
        lt = 0.0f;
    if (rt < 0.0f)
        rt = 0.0f;
    if (lt > axes[PAD_LT])
        axes[PAD_LT] = lt;
    if (rt > axes[PAD_RT])
        axes[PAD_RT] = rt;

    buttons[PAD_L2] |= lt > 0.5f;
    buttons[PAD_R2] |= rt > 0.5f;
    int hat_x = (buttons[PAD_RIGHT] ? 1 : 0) -
                (buttons[PAD_LEFT] ? 1 : 0);
    int hat_y = (buttons[PAD_DOWN] ? 1 : 0) -
                (buttons[PAD_UP] ? 1 : 0);
    strongest_axis(&axes[PAD_HAT_X], (float)hat_x);
    strongest_axis(&axes[PAD_HAT_Y], (float)hat_y);
}

static void inject_key(void *env, void *player, int action, int keycode,
                       int scancode)
{
    if (!inject_event)
        return;
    void *event = pf2_jni_key_event(action, keycode, scancode);
    unsigned char accepted =
        ((unsigned char (*)(void *, void *, void *, int))inject_event)(
            env, player, event, 0);
    nx_log("input: key %d %s -> Unity accepted=%u", keycode,
           action == 0 ? "down" : "up", accepted);
}

static void inject_motion(void *env, void *player, const float *axes)
{
    if (!inject_event)
        return;
    void *event = pf2_jni_motion_event(
        axes[PAD_LX], axes[PAD_LY], axes[PAD_RX], axes[PAD_RY],
        axes[PAD_LT], axes[PAD_RT], axes[PAD_HAT_X], axes[PAD_HAT_Y]);
    unsigned char accepted =
        ((unsigned char (*)(void *, void *, void *, int))inject_event)(
            env, player, event, 0);
    nx_log("input: axes L(%.2f,%.2f) R(%.2f,%.2f) T(%.2f,%.2f)"
           " H(%.0f,%.0f) -> Unity accepted=%u",
           axes[PAD_LX], axes[PAD_LY], axes[PAD_RX], axes[PAD_RY],
           axes[PAD_LT], axes[PAD_RT], axes[PAD_HAT_X], axes[PAD_HAT_Y],
           accepted);
}

static void inject_touch(void *env, void *player, int action)
{
    if (!inject_event)
        return;
    void *event =
        pf2_jni_touch_event(action, boot_touch_x, boot_touch_y);
    unsigned char accepted =
        ((unsigned char (*)(void *, void *, void *, int))inject_event)(
            env, player, event, 0);
    nx_log("input: boot touch %.0f,%.0f %s -> Unity accepted=%u",
           boot_touch_x, boot_touch_y,
           action == 0 ? "down" : action == 1 ? "up" : "move", accepted);
}

/*
 * The visible title object is a full-screen UnityEngine.UI.Button whose
 * serialized onClick target is MenuUI.StartGamePressed().  Its scene uses the
 * newer InputSystemUIInputModule, while UnityPlayer.nativeInjectEvent feeds
 * the Android/legacy queue.  Invoke that already-wired listener for the first
 * controller A/Start: this is the same point in the original UI flow, not a
 * scene jump or an alternate entry point.
 */
static int invoke_start_game_listener(void)
{
    static int resolved;
    static void *menu_class;
    static void *find_method;
    static void *start_method;
    static void *(*runtime_invoke)(const void *, void *, void **, void **);

    if (!resolved) {
        resolved = 1;
        nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
        if (!il2cpp)
            return 0;

        void *(*domain_get)(void) =
            nx_lookup_in(il2cpp, "il2cpp_domain_get");
        const void **(*domain_get_assemblies)(void *, size_t *) =
            nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
        void *(*assembly_get_image)(const void *) =
            nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
        const char *(*image_get_name)(const void *) =
            nx_lookup_in(il2cpp, "il2cpp_image_get_name");
        void *(*class_from_name)(const void *, const char *, const char *) =
            nx_lookup_in(il2cpp, "il2cpp_class_from_name");
        const void *(*class_get_type)(void *) =
            nx_lookup_in(il2cpp, "il2cpp_class_get_type");
        void *(*type_get_object)(const void *) =
            nx_lookup_in(il2cpp, "il2cpp_type_get_object");
        void *(*class_get_method)(void *, const char *, int) =
            nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
        runtime_invoke = nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");

        if (!domain_get || !domain_get_assemblies || !assembly_get_image ||
            !image_get_name || !class_from_name || !class_get_type ||
            !type_get_object || !class_get_method || !runtime_invoke)
            return 0;

        size_t count = 0;
        const void **assemblies =
            domain_get_assemblies(domain_get(), &count);
        void *game_image = NULL;
        void *core_image = NULL;
        for (size_t i = 0; i < count; i++) {
            void *image = assembly_get_image(assemblies[i]);
            const char *name = image ? image_get_name(image) : NULL;
            if (!name)
                continue;
            if (strcmp(name, "Assembly-CSharp.dll") == 0)
                game_image = image;
            else if (strcmp(name, "UnityEngine.CoreModule.dll") == 0)
                core_image = image;
        }
        if (!game_image || !core_image)
            return 0;

        menu_class = class_from_name(game_image, "", "MenuUI");
        void *object_class =
            class_from_name(core_image, "UnityEngine", "Object");
        if (!menu_class || !object_class)
            return 0;

        start_method =
            class_get_method(menu_class, "StartGamePressed", 0);
        find_method =
            class_get_method(object_class, "FindObjectOfType", 2);
        if (!start_method || !find_method)
            return 0;

        /* Cache the System.Type object in a spare static pointer by using the
         * class variable's companion below on every call; type_get_object is
         * cheap and the returned reflection object is managed by IL2CPP. */
        (void)type_get_object;
        nx_log("input: resolved original MenuUI.StartGamePressed listener");
    }

    if (!menu_class || !find_method || !start_method || !runtime_invoke)
        return 0;

    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    const void *(*class_get_type)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_type");
    void *(*type_get_object)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_type_get_object");
    void *menu_type = type_get_object(class_get_type(menu_class));
    unsigned char include_inactive = 0;
    void *find_args[] = { menu_type, &include_inactive };
    void *exception = NULL;
    void *instance =
        runtime_invoke(find_method, NULL, find_args, &exception);
    if (exception || !instance) {
        nx_log("input: active MenuUI not found (exception=%p)", exception);
        return 0;
    }

    exception = NULL;
    (void)runtime_invoke(start_method, instance, NULL, &exception);
    if (exception) {
        nx_log("input: MenuUI.StartGamePressed raised %p", exception);
        return 0;
    }
    nx_log("input: original Start Game button listener invoked by controller");
    show_menu_cursor();
    nx_log("input: pointer enabled for the menu");
    select_gamepad_pending = 1;
    select_gamepad_after_ms = monotonic_ms() + 1500;
    return 1;
}

/* The level1 Gamepad Button's serialized persistent call is precisely
 * Tutorial.controlsSelected(1).  Invoke that target after the user confirms
 * with A/Start, including the game's own mode setup and persistence. */
static int invoke_gamepad_button(void)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;

    void *(*domain_get)(void) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get");
    const void **(*domain_get_assemblies)(void *, size_t *) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    void *(*assembly_get_image)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    const char *(*image_get_name)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_image_get_name");
    void *(*class_from_name)(const void *, const char *, const char *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    const void *(*class_get_type)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_type");
    void *(*type_get_object)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_type_get_object");
    void *(*class_get_method)(void *, const char *, int) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    void *(*runtime_invoke)(const void *, void *, void **, void **) =
        nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    if (!domain_get || !domain_get_assemblies || !assembly_get_image ||
        !image_get_name || !class_from_name || !class_get_type ||
        !type_get_object || !class_get_method || !runtime_invoke)
        return 0;

    size_t count = 0;
    const void **assemblies =
        domain_get_assemblies(domain_get(), &count);
    void *core_image = NULL;
    void *game_image = NULL;
    for (size_t i = 0; i < count; i++) {
        void *image = assembly_get_image(assemblies[i]);
        const char *name = image ? image_get_name(image) : NULL;
        if (!name)
            continue;
        if (strcmp(name, "UnityEngine.CoreModule.dll") == 0)
            core_image = image;
        else if (strcmp(name, "Assembly-CSharp.dll") == 0)
            game_image = image;
    }
    if (!core_image || !game_image)
        return 0;

    void *object_class =
        class_from_name(core_image, "UnityEngine", "Object");
    void *tutorial_class =
        class_from_name(game_image, "", "Tutorial");
    if (!object_class || !tutorial_class)
        return 0;
    void *find_method =
        class_get_method(object_class, "FindObjectOfType", 2);
    void *selected_method =
        class_get_method(tutorial_class, "controlsSelected", 1);
    if (!find_method || !selected_method)
        return 0;

    void *tutorial_type =
        type_get_object(class_get_type(tutorial_class));
    unsigned char include_inactive = 1;
    void *find_args[] = { tutorial_type, &include_inactive };
    void *exception = NULL;
    void *tutorial =
        runtime_invoke(find_method, NULL, find_args, &exception);
    if (exception || !tutorial) {
        nx_log("input: Tutorial control selector not found (exception=%p)",
               exception);
        return 0;
    }

    int gamepad_mode = 1; /* serialized m_IntArgument of the Gamepad Button */
    void *selected_args[] = { &gamepad_mode };
    exception = NULL;
    (void)runtime_invoke(selected_method, tutorial, selected_args, &exception);
    if (exception) {
        nx_log("input: Tutorial.controlsSelected(1) raised %p", exception);
        return 0;
    }
    nx_log("input: original Bluetooth Gamepad listener controlsSelected(1) "
           "invoked");
    show_menu_cursor();
    return 1;
}

static void begin_boot_touch(void *env, void *player, unsigned long frame)
{
    if (!boot_touch_pending)
        return;
    /* A controller selected the port in EmulationStation immediately before
     * this process started, so its initial axis/button state can reach frame
     * zero.  IL2CPP's domain is not available to reflection until the first
     * Unity updates have run (the same boundary used for InputSystem device
     * registration below).  Before that boundary, stay on Android's native
     * touch path instead of entering il2cpp_domain_get prematurely. */
    if (managed_registration_allowed && invoke_start_game_listener()) {
        boot_touch_pending = 0;
        return;
    }
    boot_touch_pending = 0;
    show_menu_cursor();
    inject_touch(env, player, 0);
    /* Unity copies input into a queue drained by the UI update.  Keep the
     * finger down across several UI frames, as a real Android tap would. */
    touch_next_move_frame = frame + 4;
    touch_release_frame = frame + 24;
}

int pf2_input_init(void)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        controllers[i].instance = -1;
    for (int i = 0; i < MAX_RAW_PADS; i++)
        raw_pads[i].fd = -1;

    inject_event =
        pf2_jni_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
    const char *raw = getenv("PF2_RAW_GAMEPAD");
    force_raw = (raw && *raw && strcmp(raw, "0") != 0) ||
                (getenv("PF2_GAMEPAD") && *getenv("PF2_GAMEPAD"));

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    if (!force_raw &&
        SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) == 0) {
        sdl_ready = 1;
        SDL_GameControllerEventState(SDL_ENABLE);
        load_mapping_file(getenv("SDL_GAMECONTROLLERCONFIG_FILE"));
        load_mapping_file(
            "/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt");
        load_mapping_file(
            "/storage/.config/SDL-GameControllerDB/gamecontrollerdb-SDL2.txt");
        install_known_mappings();
        rescan_controllers();
    } else if (!force_raw) {
        nx_log("input: SDL GameController unavailable: %s", SDL_GetError());
    }

    rescan_raw_pads();
    const char *value = getenv("PF2_AUTOKEY");
    auto_key = value && *value ? atoi(value) : 0;
    value = getenv("PF2_AUTOSTART");
    auto_start = value && *value && strcmp(value, "0") != 0;
    value = getenv("PF2_CURSOR_X");
    if (value && *value)
        cursor_x = strtof(value, NULL);
    value = getenv("PF2_CURSOR_Y");
    if (value && *value)
        cursor_y = strtof(value, NULL);
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > 1279.0f) cursor_x = 1279.0f;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > 719.0f) cursor_y = 719.0f;
    value = getenv("PF2_CURSOR_SPEED");
    if (value && *value) {
        char *end = NULL;
        errno = 0;
        float requested_speed = strtof(value, &end);
        if (!errno && end != value && end && *end == '\0' &&
            isfinite(requested_speed) && requested_speed >= 200.0f &&
            requested_speed <= 5000.0f) {
            cursor_speed = requested_speed;
        } else {
            nx_log("input: ignoring invalid PF2_CURSOR_SPEED=%s", value);
        }
    }
    value = getenv("PF2_CURSOR_ALWAYS");
    if (value && *value && strcmp(value, "0") != 0)
        cursor_visible_until_ms = UINT64_MAX;
    value = getenv("PF2_AUTO_CURSOR_CLICK_FRAME");
    if (value && *value) {
        auto_cursor_click_frame = strtoul(value, NULL, 10);
        if (auto_cursor_click_frame > 30)
            auto_cursor_prime_frame = auto_cursor_click_frame - 30;
    }
    value = getenv("PF2_CURSOR_LOCK");
    cursor_locked = value && *value && strcmp(value, "0") != 0;
    value = getenv("PF2_BOOT_TOUCH");
    boot_touch_pending = !value || !*value || strcmp(value, "0") != 0;
    value = getenv("PF2_START_X");
    if (value && *value)
        boot_touch_x = strtof(value, NULL);
    value = getenv("PF2_START_Y");
    if (value && *value)
        boot_touch_y = strtof(value, NULL);
    nx_log("input: ready (SDL controllers=%d, raw pads=%d, inject=%p, "
           "cursor=%.0f px/s)",
           controller_count(),
           (raw_pads[0].fd >= 0) + (raw_pads[1].fd >= 0) +
           (raw_pads[2].fd >= 0) + (raw_pads[3].fd >= 0) +
           (raw_pads[4].fd >= 0) + (raw_pads[5].fd >= 0) +
           (raw_pads[6].fd >= 0) + (raw_pads[7].fd >= 0),
           inject_event, cursor_speed);
    return inject_event != NULL;
}

void pf2_input_poll(void *env, void *player, unsigned long frame)
{
    /* The package's InputSystem static manager is created by the first Unity
     * updates, not by nativeResume.  Register after those updates have run. */
    if (frame == 120) {
        managed_registration_allowed = 1;
        (void)ensure_managed_gamepad();
    }

    if (touch_release_frame) {
        if (frame >= touch_release_frame) {
            inject_touch(env, player, 1);
            touch_release_frame = 0;
            touch_next_move_frame = 0;
        } else if (frame >= touch_next_move_frame) {
            inject_touch(env, player, 2);
            touch_next_move_frame = frame + 4;
        }
    }

    /* Diagnostic only: exercise one normal key, or tap the title after it
     * settles.  AUTOSTART deliberately sends no gamepad event first: Prize
     * Fighters switches control schemes on the first device activity. */
    if (auto_key && frame == 300)
        inject_key(env, player, 0, auto_key, 0);
    if (auto_key && frame == 304)
        inject_key(env, player, 1, auto_key, 0);
    if (auto_start && frame == 300)
        begin_boot_touch(env, player, frame);
    if (select_gamepad_pending && managed_registration_allowed &&
        monotonic_ms() >= select_gamepad_after_ms) {
        if (!invoke_gamepad_button())
            nx_log("input: no control-selector listener active; continuing");
        select_gamepad_pending = 0;
    }
    /* Diagnostic gameplay proof: hold D-pad up through the managed Input
     * System for two seconds, then release. */
    if (auto_start && (frame == 700 || frame == 760)) {
        unsigned char test_buttons[PAD_BUTTON_COUNT] = {0};
        float test_axes[PAD_AXIS_COUNT] = {0};
        if (frame == 700)
            test_buttons[PAD_UP] = 1;
        queue_managed_state(test_buttons, test_axes);
    }
    if (auto_cursor_prime_frame && frame >= auto_cursor_prime_frame) {
        nx_log("input: diagnostic cursor primed at design (%.0f,%.0f)",
               cursor_x, cursor_y);
        queue_managed_mouse(0.0f, 0.0f, 0);
        auto_cursor_prime_frame = 0;
    }
    if (auto_cursor_click_frame && frame >= auto_cursor_click_frame) {
        nx_log("input: diagnostic cursor click at design (%.0f,%.0f)",
               cursor_x, cursor_y);
        queue_managed_mouse(0.0f, 0.0f, 1);
        auto_cursor_release_frame = frame + 12;
        auto_cursor_click_frame = 0;
    }
    if (auto_cursor_release_frame && frame >= auto_cursor_release_frame) {
        queue_managed_mouse(0.0f, 0.0f, 0);
        nx_log("input: diagnostic cursor released");
        auto_cursor_release_frame = 0;
    }

    if (sdl_ready) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                open_controller_index(event.cdevice.which);
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (int i = 0; i < MAX_CONTROLLERS; i++)
                    if (controllers[i].handle &&
                        controllers[i].instance == event.cdevice.which)
                        close_controller_slot(i);
            } else if (event.type == SDL_CONTROLLERDEVICEREMAPPED) {
                nx_log("input: controller mapping updated (instance=%d)",
                       event.cdevice.which);
            } else if (event.type == SDL_QUIT) {
                exit_to_frontend();
            }
        }
        SDL_GameControllerUpdate();
    }

    if (frame % 120 == 0) {
        if (sdl_ready)
            rescan_controllers();
        rescan_raw_pads();
    }

    unsigned char buttons[PAD_BUTTON_COUNT] = {0};
    float axes[PAD_AXIS_COUNT] = {0};
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].handle)
            merge_controller(controllers[i].handle, buttons, axes);
    for (int i = 0; i < MAX_RAW_PADS; i++) {
        pump_raw_events(&raw_pads[i]);
        merge_raw_pad(&raw_pads[i], buttons, axes);
    }
    if ((buttons[PAD_BACK] || controller_guide_pressed()) &&
        buttons[PAD_START])
        exit_to_frontend();

    float cursor_delta_x = 0.0f;
    float cursor_delta_y = 0.0f;
    int cursor_moved =
        update_virtual_cursor(axes, &cursor_delta_x, &cursor_delta_y);
    int cursor_r3 = buttons[PAD_R3] != 0;
    int cursor_r3_click = cursor_r3 && !cursor_r3_previous;
    cursor_r3_previous = cursor_r3;
    int cursor_a = buttons[PAD_A] != 0;
    int cursor_a_click = cursor_a && !cursor_a_previous;
    cursor_a_previous = cursor_a;
    int cursor_start = buttons[PAD_START] != 0;
    int cursor_start_click = cursor_start && !cursor_start_previous;
    cursor_start_previous = cursor_start;
    if (cursor_r3_click)
        show_menu_cursor();
    if (cursor_start_click)
        show_menu_cursor();

    int gameplay_activity =
        fabsf(axes[PAD_LX]) > 0.45f ||
        fabsf(axes[PAD_LY]) > 0.45f ||
        buttons[PAD_B] || buttons[PAD_X] || buttons[PAD_Y] ||
        buttons[PAD_L1] || buttons[PAD_R1] ||
        buttons[PAD_L2] || buttons[PAD_R2] ||
        buttons[PAD_UP] || buttons[PAD_DOWN] ||
        buttons[PAD_LEFT] || buttons[PAD_RIGHT];
    if (!keyboard_active && cursor_menu_visible && !cursor_moved &&
        gameplay_activity) {
        hide_menu_cursor();
        nx_log("input: pointer hidden after gameplay/controller activity");
    }
    int cursor_visible = pf2_input_cursor(NULL, NULL);
    int keyboard_click = cursor_r3_click ||
                         (cursor_visible && cursor_a_click);
    int keyboard_owned = keyboard_active;
    if (keyboard_owned) {
        /* Like the proven FF4 naming keyboard, text entry owns the pad.  It
         * can be driven by right-stick/R3 or D-pad/A; B erases and Start
         * finishes.  No underlying Unity button receives the same press. */
        keyboard_handle(buttons, cursor_moved, keyboard_click);
        memset(buttons, 0, sizeof buttons);
        memset(axes, 0, sizeof axes);
    } else {
        /* The original Mali-450 build proved Unity's managed Mouse path end
         * to end.  R36S pads often have no SDL R3 mapping, so visible-pointer
         * A is an alias for that exact same mouse button, not a second UI
         * implementation. */
        int cursor_a_hit = 0;
        if (cursor_visible && cursor_a_click &&
            !boot_touch_pending && !select_gamepad_pending &&
            managed_registration_allowed)
            cursor_a_hit = ui_cursor_update(0);
        if (cursor_a_hit) {
            cursor_a_captured = 1;
            show_menu_cursor();
        } else if (cursor_visible && cursor_a_click &&
                   cursor_menu_visible && !boot_touch_pending &&
                   !select_gamepad_pending) {
            /* Outside an active uGUI button, A is gameplay input.  Retire the
             * pointer before forwarding the same press to the Gamepad. */
            hide_menu_cursor();
            cursor_visible = pf2_input_cursor(NULL, NULL);
        }
        int cursor_a_owned = cursor_a_captured;
        int cursor_mouse_down = cursor_r3 ||
                                (cursor_a_captured && cursor_a);
        int cursor_button_changed =
            cursor_mouse_down != cursor_mouse_previous;
        cursor_mouse_previous = cursor_mouse_down;

        /* Do not resolve uGUI classes from the controller's startup axis
         * sample.  The IL2CPP domain only becomes reflection-safe after the
         * first Unity updates, at managed_registration_allowed. */
        if (managed_registration_allowed &&
            (cursor_moved || cursor_button_changed ||
             (pf2_input_cursor(NULL, NULL) && frame % 6 == 0)))
            (void)ui_cursor_update(0);
        if (cursor_moved || cursor_button_changed)
            queue_managed_mouse(cursor_delta_x, cursor_delta_y,
                                cursor_mouse_down);
        if (cursor_button_changed)
            nx_log("input: pointer mouse %s via %s at (%.0f,%.0f)",
                   cursor_mouse_down ? "down" : "up",
                   cursor_r3 ? "R3" : "A", cursor_x, cursor_y);

        /* A captured by the pointer stays out of the Gamepad stream through
         * its release frame, preventing one physical press from firing both
         * the UI pointer and a gameplay action. */
        if (cursor_a_owned) {
            buttons[PAD_A] = 0;
            if (!cursor_a)
                cursor_a_captured = 0;
        }

        /* The right stick and R3 belong to the pointer.  Do not also deliver
         * them as a second InputSystem action stream. */
        buttons[PAD_R3] = 0;
        axes[PAD_RX] = 0.0f;
        axes[PAD_RY] = 0.0f;
    }

    int managed_changed = 0;
    for (int i = 0; i < PAD_BUTTON_COUNT; i++) {
        if (buttons[i] == previous_buttons[i])
            continue;
        managed_changed = 1;

        int boot_button = i == PAD_A || i == PAD_START;
        if (buttons[i] && boot_button && select_gamepad_pending) {
            if (invoke_gamepad_button()) {
                swallowed_boot_buttons[i] = 1;
            } else {
                /* Some saves have already completed the first-run selector.
                 * Do not consume every future A/Start when it is absent. */
                nx_log("input: control selector absent; forwarding button");
                if (!managed_gamepad)
                    inject_key(env, player, 0, android_keycode[i], i);
            }
            select_gamepad_pending = 0;
        } else if (buttons[i] && boot_button &&
            (boot_touch_pending || touch_release_frame)) {
            /* The title is touch-only.  Consume the first A/Start pair so it
             * cannot select the Gamepad control scheme before uGUI receives
             * the complete DOWN/MOVE/UP gesture. */
            swallowed_boot_buttons[i] = 1;
            begin_boot_touch(env, player, frame);
        } else if (!buttons[i] && swallowed_boot_buttons[i]) {
            swallowed_boot_buttons[i] = 0;
        } else {
            /* nativeInjectEvent is the compatibility fallback.  Once the
             * game's actual InputSystem Gamepad exists, one authoritative
             * state stream avoids duplicate button callbacks. */
            if (!managed_gamepad)
                inject_key(env, player, buttons[i] ? 0 : 1,
                           android_keycode[i], i);
        }
        previous_buttons[i] = buttons[i];
    }

    /* Do not inject a neutral joystick event at boot: it would switch the
     * title away from its touchscreen path before the first A/Start tap. */
    int motion_changed = 0;
    for (int i = 0; i < PAD_AXIS_COUNT; i++)
        if (fabsf(axes[i] - previous_axes[i]) > 0.0001f) {
            motion_changed = 1;
            break;
        }
    if (motion_changed) {
        if (!managed_gamepad)
            inject_motion(env, player, axes);
        memcpy(previous_axes, axes, sizeof previous_axes);
    }
    if (managed_changed || motion_changed) {
        unsigned char managed_buttons[PAD_BUTTON_COUNT];
        memcpy(managed_buttons, buttons, sizeof managed_buttons);
        for (int i = 0; i < PAD_BUTTON_COUNT; i++)
            if (swallowed_boot_buttons[i])
                managed_buttons[i] = 0;
        queue_managed_state(managed_buttons, axes);
    }
}

void pf2_input_close(void)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        close_controller_slot(i);
    close_all_raw_pads();
    if (sdl_ready) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                          SDL_INIT_EVENTS);
        sdl_ready = 0;
    }
}
