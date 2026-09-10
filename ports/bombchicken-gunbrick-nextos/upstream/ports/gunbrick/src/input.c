/* SPDX-License-Identifier: GPL-3.0-only */
/* Gunbrick controller/touch adapter. Guest code and fixed guest offsets are
 * never modified here: SDL events are observed by nxinput and forwarded
 * through the Android input lifecycle registered by this exact Unity player. */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"
#include "framework_bridge.h"
#include "nxinput.h"

#define GB_SWIPE_QUIRK "game.gunbrick.swipe-input"

static nxinput_context *gb_input;
static volatile sig_atomic_t gb_exit_requested;
static uint32_t gb_previous_buttons;
static int gb_previous_connected;
static uint32_t gb_reported_generation;
static int gb_screen_width = 1280;
static int gb_screen_height = 720;
static int gb_input_diag;
static int gb_swipe_enabled;
static int gb_swipe_latched;
static unsigned gb_swipe_step;
static float gb_swipe_x;
static float gb_swipe_y;
static float gb_swipe_dx;
static float gb_swipe_dy;

static const int gb_android_key[NXINPUT_BUTTON_COUNT] = {
    [NXINPUT_BUTTON_A] = 96,
    [NXINPUT_BUTTON_B] = 97,
    [NXINPUT_BUTTON_X] = 99,
    [NXINPUT_BUTTON_Y] = 100,
    [NXINPUT_BUTTON_BACK] = 109,
    [NXINPUT_BUTTON_GUIDE] = 110,
    [NXINPUT_BUTTON_START] = 108,
    [NXINPUT_BUTTON_LEFT_STICK] = 106,
    [NXINPUT_BUTTON_RIGHT_STICK] = 107,
    [NXINPUT_BUTTON_LEFT_SHOULDER] = 102,
    [NXINPUT_BUTTON_RIGHT_SHOULDER] = 103,
    [NXINPUT_BUTTON_DPAD_UP] = 19,
    [NXINPUT_BUTTON_DPAD_DOWN] = 20,
    [NXINPUT_BUTTON_DPAD_LEFT] = 21,
    [NXINPUT_BUTTON_DPAD_RIGHT] = 22,
};

static void gb_inject(void *env, void *player, void *event);

/* ===== Gamepad cursor (shared fleet behavior) =====
 * The game's menus are touch-only: without a pointer the title screen is a
 * frozen picture.  Right stick moves the arrow, R3 press touches.  The arrow
 * auto-hides after a few idle seconds; the click keeps working. */
static int gb_cursor_enabled = 1;
static float gb_cursor_x = 640.0f;
static float gb_cursor_y = 360.0f;
static float gb_cursor_vx;
static float gb_cursor_vy;
static uint64_t gb_cursor_tick;
static uint64_t gb_cursor_seen_tick;
static float gb_cursor_hide_after = 4.0f;
static int gb_cursor_drag_active;
static int gb_cursor_click_prev;
static float gb_cursor_touch_x;
static float gb_cursor_touch_y;

static void gb_update_cursor(void *env, void *player,
                             const nxinput_pad_state *pad)
{
    if (!gb_cursor_enabled || !pad || !pad->connected)
        return;

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = gb_cursor_tick && frequency
             ? (float)((double)(now - gb_cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    gb_cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = pad->right_x;
    float y = pad->right_y;
    float magnitude = sqrtf(x * x + y * y);
    float target_x = 0.0f;
    float target_y = 0.0f;
    const float deadzone = 0.18f;
    if (magnitude > deadzone) {
        float response = (magnitude - deadzone) / (1.0f - deadzone);
        if (response > 1.0f)
            response = 1.0f;
        response *= response;
        target_x = x / magnitude * response * 1050.0f;
        target_y = y / magnitude * response * 1050.0f;
    }

    int held = (pad->buttons &
                NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_RIGHT_STICK)) != 0u;
    if (magnitude > deadzone || held)
        gb_cursor_seen_tick = now;   /* moved or clicked: arrow reappears */

    float blend = 1.0f - expf(-14.0f * dt);
    gb_cursor_vx += (target_x - gb_cursor_vx) * blend;
    gb_cursor_vy += (target_y - gb_cursor_vy) * blend;
    gb_cursor_x += gb_cursor_vx * dt;
    gb_cursor_y += gb_cursor_vy * dt;
    if (gb_cursor_x < 0.0f) gb_cursor_x = 0.0f;
    if (gb_cursor_x > 1279.0f) gb_cursor_x = 1279.0f;
    if (gb_cursor_y < 0.0f) gb_cursor_y = 0.0f;
    if (gb_cursor_y > 719.0f) gb_cursor_y = 719.0f;

    int down = held && !gb_cursor_click_prev;
    int up = !held && gb_cursor_click_prev;
    gb_cursor_click_prev = held;
    float touch_x = gb_cursor_x * (float)gb_screen_width / 1280.0f;
    float touch_y = gb_cursor_y * (float)gb_screen_height / 720.0f;
    if (down) {
        gb_inject(env, player, gb_jni_touch_event(0, touch_x, touch_y));
        gb_cursor_drag_active = 1;
        gb_cursor_touch_x = touch_x;
        gb_cursor_touch_y = touch_y;
        if (gb_input_diag)
            fprintf(stderr, "[gb/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && gb_cursor_drag_active &&
               (fabsf(touch_x - gb_cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - gb_cursor_touch_y) >= 0.25f)) {
        gb_inject(env, player, gb_jni_touch_event(2, touch_x, touch_y));
        gb_cursor_touch_x = touch_x;
        gb_cursor_touch_y = touch_y;
    }
    if (up && gb_cursor_drag_active) {
        gb_inject(env, player, gb_jni_touch_event(1, touch_x, touch_y));
        gb_cursor_drag_active = 0;
        if (gb_input_diag)
            fprintf(stderr, "[gb/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
    }
}

static int gb_named_list_contains(const char *values, const char *expected)
{
    const char *cursor;
    size_t expected_length;
    if (!values || !expected || !*expected)
        return 0;
    expected_length = strlen(expected);
    cursor = values;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length == expected_length &&
            memcmp(cursor, expected, expected_length) == 0)
            return 1;
        if (!end)
            break;
        cursor = end + 1;
    }
    return 0;
}

static void gb_inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = gb_jni_native("com/unity3d/player/UnityPlayer",
                                      "nativeInjectEvent");
    if (native_inject && event) {
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (gb_input_diag)
            fprintf(stderr, "[gb/input] event=%p consumed=%u\n", event,
                    (unsigned)consumed);
    }
}

void gb_input_request_exit(void)
{
    gb_exit_requested = 1;
}

static void gb_release_buttons(void *env, void *player)
{
    unsigned index;
    for (index = 0; index < (unsigned)NXINPUT_BUTTON_COUNT; ++index) {
        uint32_t bit = NXINPUT_BUTTON_BIT(index);
        if ((gb_previous_buttons & bit) != 0u && gb_android_key[index] != 0)
            gb_inject(env, player,
                      gb_jni_key_event(1, gb_android_key[index], (int)index));
    }
    gb_previous_buttons = 0u;
}

static void gb_forward_buttons(void *env, void *player, uint32_t buttons)
{
    unsigned index;
    uint32_t changed = buttons ^ gb_previous_buttons;
    for (index = 0; index < (unsigned)NXINPUT_BUTTON_COUNT; ++index) {
        uint32_t bit = NXINPUT_BUTTON_BIT(index);
        if ((changed & bit) == 0u || gb_android_key[index] == 0)
            continue;
        gb_inject(env, player,
                  gb_jni_key_event((buttons & bit) != 0u ? 0 : 1,
                                   gb_android_key[index], (int)index));
    }
    gb_previous_buttons = buttons;
}

static void gb_finish_swipe(void *env, void *player)
{
    if (gb_swipe_step != 0u)
        gb_inject(env, player,
                  gb_jni_touch_event(1, gb_swipe_x + gb_swipe_dx,
                                     gb_swipe_y + gb_swipe_dy));
    gb_swipe_step = 0u;
}

static void gb_update_swipe(void *env, void *player,
                            const nxinput_pad_state *pad)
{
    float x;
    float y;
    float magnitude;
    float length;

    if (!gb_swipe_enabled || !pad || !pad->connected) {
        gb_finish_swipe(env, player);
        gb_swipe_latched = 0;
        return;
    }

    if (gb_swipe_step != 0u) {
        float progress = (float)gb_swipe_step / 4.0f;
        int action = gb_swipe_step >= 4u ? 1 : 2;
        gb_inject(env, player,
                  gb_jni_touch_event(action,
                                     gb_swipe_x + gb_swipe_dx * progress,
                                     gb_swipe_y + gb_swipe_dy * progress));
        gb_swipe_step++;
        if (gb_swipe_step > 4u)
            gb_swipe_step = 0u;
        return;
    }

    x = pad->left_x;
    y = pad->left_y;
    if ((pad->buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT)) != 0u)
        x = 1.0f;
    else if ((pad->buttons &
              NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT)) != 0u)
        x = -1.0f;
    if ((pad->buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_DOWN)) != 0u)
        y = 1.0f;
    else if ((pad->buttons &
              NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_UP)) != 0u)
        y = -1.0f;

    magnitude = sqrtf(x * x + y * y);
    if (magnitude < 0.35f) {
        gb_swipe_latched = 0;
        return;
    }
    if (gb_swipe_latched || magnitude < 0.65f)
        return;

    gb_swipe_latched = 1;
    length = 0.22f * (float)(gb_screen_width < gb_screen_height
                                 ? gb_screen_width
                                 : gb_screen_height);
    gb_swipe_x = (float)gb_screen_width * 0.5f;
    gb_swipe_y = (float)gb_screen_height * 0.55f;
    gb_swipe_dx = x / magnitude * length;
    gb_swipe_dy = y / magnitude * length;
    gb_inject(env, player,
              gb_jni_touch_event(0, gb_swipe_x, gb_swipe_y));
    gb_swipe_step = 1u;
    if (gb_input_diag)
        fprintf(stderr, "[gb/input] bounded swipe %.2f %.2f\n", x, y);
}

int gb_input_init(void)
{
    nxinput_config config;
    const char *quirks = getenv("NXCOMPAT_ENABLED_QUIRKS");
    gb_input_diag = getenv("GB_INPUT_DIAG") != NULL;
    gb_cursor_enabled = !(getenv("GB_CURSOR") &&
                          strcmp(getenv("GB_CURSOR"), "0") == 0);
    gb_swipe_enabled = gb_named_list_contains(quirks, GB_SWIPE_QUIRK);
    nxinput_config_init(&config);
    config.initialize_sdl = 1;
    config.rescan_interval_ms = 1000u;
    gb_input = nxinput_create(&config);
    if (!gb_input) {
        fprintf(stderr, "[gb/input] nxinput create failed: %s\n",
                SDL_GetError());
        return -1;
    }
    nxinput_set_cursor_context(gb_input, NXINPUT_CURSOR_OFF);
    if (gb_framework_publish_input(gb_input) != 0) {
        nxinput_destroy(gb_input);
        gb_input = NULL;
        fprintf(stderr,
                "[gb/input] nxinput capability publication failed\n");
        return -1;
    }
    fprintf(stderr,
            "[gb/input] nxinput active controllers=%u swipe_quirk=%s cursor=%s\n",
            nxinput_connected_count(gb_input),
            gb_swipe_enabled ? "enabled" : "disabled",
            gb_cursor_enabled ? "on (right stick + R3)" : "off");
    return 0;
}

void gb_input_poll(void *env, void *player, unsigned long frame)
{
    nxinput_pad_state pad;
    SDL_Event event;
    int slot;

    if (!gb_input)
        return;
    while (SDL_PollEvent(&event)) {
        nxinput_observe_event(gb_input, &event);
        if (event.type == SDL_QUIT)
            gb_exit_requested = 1;
    }
    nxinput_poll(gb_input);
    if (nxinput_consume_quit_request(gb_input))
        gb_exit_requested = 1;

    memset(&pad, 0, sizeof(pad));
    slot = nxinput_first_connected(gb_input);
    if (slot < 0 ||
        nxinput_get_pad(gb_input, (unsigned)slot, &pad) == 0 ||
        !pad.connected) {
        if (gb_previous_connected) {
            gb_release_buttons(env, player);
            gb_inject(env, player,
                      gb_jni_motion_event(0.0f, 0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0, 0));
        }
        gb_previous_connected = 0;
        gb_finish_swipe(env, player);
        return;
    }

    gb_previous_connected = 1;
    if (pad.generation != gb_reported_generation) {
        gb_jni_input_device_info(pad.name[0] ? pad.name : "SDL Controller",
                                 0, 0,
                                 pad.guid[0] ? pad.guid : "nxinput");
        gb_reported_generation = pad.generation;
    }

    gb_forward_buttons(env, player, pad.buttons);
    gb_inject(env, player,
              gb_jni_motion_event(pad.left_x, pad.left_y,
                                  pad.right_x, pad.right_y,
                                  pad.left_trigger, pad.right_trigger,
                                  ((pad.buttons & NXINPUT_BUTTON_BIT(
                                        NXINPUT_BUTTON_DPAD_RIGHT)) != 0u) -
                                      ((pad.buttons & NXINPUT_BUTTON_BIT(
                                        NXINPUT_BUTTON_DPAD_LEFT)) != 0u),
                                  ((pad.buttons & NXINPUT_BUTTON_BIT(
                                        NXINPUT_BUTTON_DPAD_DOWN)) != 0u) -
                                      ((pad.buttons & NXINPUT_BUTTON_BIT(
                                        NXINPUT_BUTTON_DPAD_UP)) != 0u)));
    gb_update_swipe(env, player, &pad);
    gb_update_cursor(env, player, &pad);

    if (gb_input_diag && frame != 0u && frame % 600u == 0u)
        fprintf(stderr,
                "[gb/input] frame=%lu controllers=%u generation=%u\n",
                frame, nxinput_connected_count(gb_input), pad.generation);
}

void gb_input_close(void)
{
    nxinput_destroy(gb_input);
    gb_input = NULL;
    gb_previous_buttons = 0u;
    gb_previous_connected = 0;
    gb_swipe_step = 0u;
}

int gb_input_exit_requested(void)
{
    return gb_exit_requested != 0;
}

int gb_input_cursor(float *x, float *y)
{
    if (!gb_cursor_enabled || !gb_previous_connected)
        return 0;
    if (gb_cursor_hide_after > 0.0f && gb_cursor_seen_tick) {
        uint64_t frequency = SDL_GetPerformanceFrequency();
        float idle = frequency
            ? (float)((double)(SDL_GetPerformanceCounter() -
                               gb_cursor_seen_tick) / (double)frequency)
            : 0.0f;
        if (idle > gb_cursor_hide_after)
            return 0;
    } else if (!gb_cursor_seen_tick) {
        return 0;   /* never touched: keep the picture clean until used */
    }
    if (x) *x = gb_cursor_x;
    if (y) *y = gb_cursor_y;
    return 1;
}

void gb_input_set_screen_size(int width, int height)
{
    if (width > 0)
        gb_screen_width = width;
    if (height > 0)
        gb_screen_height = height;
}

void gb_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void gb_input_keyboard_set(const char *text)
{
    (void)text;
}

void gb_input_keyboard_hide(void)
{
}

int gb_input_keyboard_snapshot(char *text, size_t text_size,
                               int *uppercase, int *selected,
                               const gb_keyboard_key **keys,
                               size_t *key_count)
{
    if (text && text_size)
        text[0] = '\0';
    if (uppercase)
        *uppercase = 0;
    if (selected)
        *selected = 0;
    if (keys)
        *keys = NULL;
    if (key_count)
        *key_count = 0u;
    return 0;
}
